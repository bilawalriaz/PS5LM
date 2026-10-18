// The control port (control.hpp): a small HTTP/1.1 server of its own, one
// thread per connection, so a request that blocks on a stalled /data write
// holds only its own thread, and never llama-server's queue or HTTP pool.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

#include "version.hpp"

#ifdef __PROSPERO__
extern "C" int sceSystemServiceLoadExec(const char * path, const char * const * args);
#else
// On a PC (ps5/app/control_host.cpp) a relaunch is an exit with 75, which
// scripts/control-host-test.sh takes as "start me again".
static int sceSystemServiceLoadExec(const char *, const char * const *) {
    std::fflush(nullptr);
    _exit(75);
}
#endif

using json = nlohmann::ordered_json;

namespace ps5lm {

namespace {

constexpr int      kMaxConnections = 16;      // a hung read holds one; past this, 503
constexpr size_t   kMaxHeader      = 16384;
constexpr size_t   kMaxBody        = 1u << 20;
constexpr size_t   kLogDefault     = 16384;
constexpr size_t   kLogMax         = 1u << 20;
constexpr double   kAnsweringS     = 5;       // /metrics answered this recently: the main loop runs
constexpr double   kWedgedS        = 15;      // not for this long, with a model up: wedged
constexpr int      kFallbackCleanS = 30;      // the clean stop waits 10 s, then the main loop LoadExecs
constexpr int      kFallbackHardS  = 8;
constexpr int      kFileTimeoutMs  = 3000;    // a write to /data that takes longer is left behind

double steady() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A thread with its own stack: the console's default is small, and JSON is deep.
void spawn(std::function<void()> fn, size_t stack = 1u << 20) {
    auto * heap = new std::function<void()>(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t{};
    if (pthread_create(&t, &attr, [](void * p) -> void * {
            std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(p));
            (*f)();
            return nullptr;
        }, heap) != 0) {
        delete heap;
    }
    pthread_attr_destroy(&attr);
}

// Runs `fn` on a thread of its own and waits `ms` at most: /data can stall
// (the wedge this port exists for began as a log write that never returned).
bool bounded(std::function<void()> fn, int ms) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    spawn([fn = std::move(fn), done] {
        fn();
        *done = true;
    }, 256u << 10);
    for (int waited = 0; waited < ms && !*done; waited += 10) {
        usleep(10 * 1000);
    }
    return *done;
}

bool read_file(const std::string & path, std::string * out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool write_file(const std::string & path, const std::string & text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return (bool) out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t at = 0;
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) {
        ++at;
    }
    return s.substr(at);
}

// 32 hex digits. A LAN guard against a stray request, not a secret from
// someone who can read /data over FTP.
std::string new_token() {
    uint64_t seed = (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count() ^
                    ((uint64_t) std::time(nullptr) << 20) ^ (uint64_t) (uintptr_t) &seed;
    try {
        std::random_device rd;
        seed ^= ((uint64_t) rd() << 32) ^ rd();
    } catch (...) {
    }
    std::mt19937_64 gen(seed);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long) gen(), (unsigned long long) gen());
    return buf;
}

bool same(const std::string & a, const std::string & b) {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= (unsigned char) (a[i] ^ b[i]);
    }
    return diff == 0;
}

const char * state_name(ServerState s) {
    switch (s) {
        case ServerState::loading: return "loading";
        case ServerState::ready: return "ready";
        case ServerState::generating: return "generating";
        case ServerState::failed: return "failed";
        case ServerState::no_model: return "no_model";
    }
    return "unknown";
}

bool serving(ServerState s) { return s == ServerState::ready || s == ServerState::generating; }

// One GET over loopback with a timeout: the status code (0 when nothing
// answered in time) and how long it took.
struct Probe {
    int    status = 0;
    double ms     = 0;
};

Probe probe(int port, const char * path, int timeout_ms) {
    Probe p;
    const double t0 = steady();
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return p;
    }
    timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in addr = {};
#if defined(__FreeBSD__) || defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *) &addr, sizeof(addr)) == 0) {
        const std::string req = std::string("GET ") + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
        send(fd, req.data(), req.size(), 0);
        char buf[512];
        const ssize_t got = recv(fd, buf, sizeof(buf) - 1, 0);
        if (got > 12) {
            buf[got] = 0;
            p.status = std::atoi(buf + 9);  // "HTTP/1.1 200 OK"
        }
    }
    close(fd);
    p.ms = (steady() - t0) * 1000.0;
    return p;
}

// The query part of a target, decoded ("/logs/llama?bytes=4096").
std::map<std::string, std::string> query_of(const std::string & q) {
    std::map<std::string, std::string> out;
    std::stringstream ss(q);
    for (std::string kv; std::getline(ss, kv, '&');) {
        const size_t eq = kv.find('=');
        std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1), d;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size()) {
                d += (char) std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else {
                d += v[i] == '+' ? ' ' : v[i];
            }
        }
        out[k] = d;
    }
    return out;
}

Control::Reply json_reply(int status, const json & j) {
    Control::Reply r;
    r.status = status;
    r.body   = j.dump(2) + "\n";
    return r;
}

Control::Reply error_reply(int status, const std::string & message) { return json_reply(status, { { "error", message } }); }

// env.txt's lines, the way main() reads them: NAME=value, # comments.
bool env_name_ok(const std::string & name) {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    for (const char * prefix : { "RADV_", "MESA_", "ACO_", "GGML_", "LLAMA_" }) {
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

json env_view(const std::string & text) {
    json vars = json::object(), process = json::object();
    std::stringstream ss(text);
    for (std::string line; std::getline(ss, line);) {
        line = trim(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string name = line.substr(0, eq);
        vars[name] = line.substr(eq + 1);
        const char * now = std::getenv(name.c_str());
        process[name] = now ? json(now) : json(nullptr);
    }
    return { { "text", text },
             { "vars", vars },
             { "process", process },
             { "note", "env.txt is read when the app starts; POST /restart to apply it. Only RADV_, MESA_, ACO_, GGML_ "
                       "and LLAMA_ names are set (LLAMA_ARG_* are llama-server's options)." } };
}

json watchdog_json(const WatchdogConfig & w) {
    return { { "watchdog_s", w.watchdog_s }, { "grace_s", w.grace_s },   { "max_per_hour", w.max_per_hour },
             { "hard", w.hard },             { "on_failure", w.on_failure } };
}

// A partial watchdog object over `w`; false with `error` for a bad key.
bool patch_watchdog(const json & j, WatchdogConfig * w, std::string * error) {
    if (!j.is_object()) {
        *error = "the body is not a JSON object";
        return false;
    }
    WatchdogConfig out = *w;
    for (const auto & [key, v] : j.items()) {
        if (key == "watchdog_s" || key == "grace_s" || key == "max_per_hour") {
            if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > 86400) {
                *error = "\"" + key + "\" takes a whole number from 0 to 86400";
                return false;
            }
            (key == "watchdog_s" ? out.watchdog_s : key == "grace_s" ? out.grace_s : out.max_per_hour) = v.get<uint32_t>();
        } else if (key == "hard" || key == "on_failure") {
            if (!v.is_boolean()) {
                *error = "\"" + key + "\" takes true or false";
                return false;
            }
            (key == "hard" ? out.hard : out.on_failure) = v.get<bool>();
        } else if (key != "rearm" && key != "rearmed_unix") {
            *error = "unknown watchdog key \"" + key + "\"";
            return false;
        }
    }
    if (out.max_per_hour == 0) {
        *error = "max_per_hour is at least 1";
        return false;
    }
    if (out.watchdog_s != 0 && out.watchdog_s < 15) {
        *error = "watchdog_s is 0 (off) or at least 15";
        return false;
    }
    *w = out;
    return true;
}

}  // namespace

std::string Control::path(const char * name) const { return dir_ + "/" + name; }

void Control::start(int port, int server_port, const char * dir, StatsCollector * stats) {
    port_         = port;
    server_port_  = server_port;
    dir_          = dir;
    stats_        = stats;
    started_      = steady();
    started_unix_ = (int64_t) std::time(nullptr);
    boot_id_      = new_token().substr(0, 12);

    // The token: the file's first line, made once.
    std::string text;
    if (read_file(path("control-token.txt"), &text) && !trim(text).empty()) {
        token_ = trim(text.substr(0, text.find('\n')));
    } else {
        token_ = new_token();
        if (!write_file(path("control-token.txt"), token_ + "\n")) {
            std::printf("ps5lm-app: control: could not write control-token.txt; every request will be refused\n");
            token_.clear();
        }
    }
    // The watchdog's settings, and the restarts of the last hour.
    int64_t rearmed = 0;  // restarts before the last PATCH /watchdog do not count
    if (read_file(path("control.json"), &text)) {
        std::string error;
        WatchdogConfig w;
        const json j = json::parse(text, nullptr, false);
        if (patch_watchdog(j, &w, &error)) {
            watchdog_ = w;
            rearmed_at_ = rearmed = j.value("rearmed_unix", (int64_t) 0);
        } else {
            std::printf("ps5lm-app: control: control.json ignored: %s\n", error.c_str());
        }
    }
    if (read_file(path("control-restarts.txt"), &text)) {
        std::stringstream ss(text);
        for (std::string line; std::getline(ss, line);) {
            if (trim(line).empty()) {
                continue;
            }
            last_restart_ = trim(line);
            const int64_t at = std::strtoll(line.c_str(), nullptr, 10);
            if (at > started_unix_ - 3600 && at >= rearmed) {
                restarts_.push_back(at);
            }
        }
    }
    std::printf("ps5lm-app: control: port %d, boot %s, watchdog %s\n", port_, boot_id_.c_str(),
                watchdog_.watchdog_s ? (std::to_string(watchdog_.watchdog_s) + " s").c_str() : "off");
    spawn([this] { accept_loop(); });
    spawn([this] { watchdog_loop(); });
}

bool Control::take(ControlAction * out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (actions_.empty()) {
        return false;
    }
    *out = std::move(actions_.front());
    actions_.pop_front();
    return true;
}

void Control::post(ControlAction a) {
    std::lock_guard<std::mutex> lock(mutex_);
    actions_.push_back(std::move(a));
}

void Control::record(const std::string & reason) {
    const int64_t now = (int64_t) std::time(nullptr);
    std::string line = std::to_string(now) + " " + reason;
    for (char & c : line) {
        c = c == '\n' || c == '\r' ? ' ' : c;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        restarts_.push_back(now);
        last_restart_ = line;
    }
    const std::string file = path("control-restarts.txt");
    bounded([file, line] {
        std::ofstream(file, std::ios::app) << line << "\n";
    }, kFileTimeoutMs);
}

void Control::save_watchdog() {
    json j;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        j = watchdog_json(watchdog_);
        j["rearmed_unix"] = rearmed_at_;
    }
    const std::string file = path("control.json"), text = j.dump(2) + "\n";
    bounded([file, text] { write_file(file, text); }, kFileTimeoutMs);
}

// ---- restarting -----------------------------------------------------------------

Control::Reply Control::restart(const Restart & r) {
    if (restarting_.exchange(true)) {
        std::lock_guard<std::mutex> lock(mutex_);
        return json_reply(409, { { "error", "a restart is already under way" },
                                 { "reason", restart_reason_ },
                                 { "since_s", steady() - restart_at_ } });
    }
    const Live live = stats_->snapshot();
    auto refuse = [&](int status, const std::string & message, json extra = json::object()) {
        restarting_ = false;
        extra["error"] = message;
        return json_reply(status, extra);
    };

    // The model: a path, or a library row by its label or file name.
    std::string model;
    if (!r.model.empty()) {
        const ModelRow * row = nullptr;
        for (const auto & m : live.models) {
            const std::string file_name = m.file.substr(m.file.find_last_of('/') + 1);
            if (m.file == r.model || m.label == r.model || file_name == r.model) {
                row = &m;
            }
        }
        // The next start loads model.txt only when it names a library row
        // that fits; anything else needs its own arguments.
        if (row == nullptr && r.args.empty()) {
            json known = json::array();
            for (const auto & m : live.models) {
                known.push_back(m.label);
            }
            return refuse(404, "no model \"" + r.model + "\" in the library (a file elsewhere needs \"args\")",
                          { { "models", known } });
        }
        if (row != nullptr && !row->fits && r.args.empty()) {
            return refuse(409, "the planner says " + row->label + " does not fit: " + row->plan +
                                   " (pass \"args\" to load it anyway)");
        }
        model = row != nullptr ? row->file : r.model;
    }

    // What the next start reads: app-args.txt verbatim, else model.txt.
    std::string args_text, choice;
    if (!r.args.empty()) {
        std::vector<std::string> args = r.args;
        if (std::find(args.begin(), args.end(), "-m") == args.end()) {
            const std::string m = !model.empty() ? model : live.model_file;
            if (m.empty()) {
                return refuse(400, "\"args\" has no -m and no model is loaded or named");
            }
            args.insert(args.begin(), { "-m", m });
        }
        for (const auto & a : args) {
            if (a.find('\n') != std::string::npos) {
                return refuse(400, "an argument holds a line break");
            }
            args_text += a + "\n";
        }
    }
    if (r.unload) {
        choice = "none";
    } else if (!model.empty()) {
        choice = model;
    } else if (!live.model_file.empty()) {
        choice = live.model_file;  // the same model again
    }
    const bool drop_args = args_text.empty() && (r.clear_args || r.unload || !model.empty());

    const std::string reason = r.reason.empty() ? "control: restart" : r.reason;
    std::printf("ps5lm-app: %s (%s%s%s)\n", reason.c_str(), r.hard ? "hard" : "clean",
                choice.empty() ? "" : ", next ", choice.c_str());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        restart_at_     = steady();
        restart_reason_ = reason;
    }
    const std::string args_file = path("app-args.txt"), choice_file = path("model.txt");
    const bool written = bounded([=] {
        if (!args_text.empty()) {
            write_file(args_file, args_text);
        } else if (drop_args) {
            unlink(args_file.c_str());
        }
        if (!choice.empty()) {
            write_file(choice_file, choice + "\n");
        }
    }, kFileTimeoutMs);
    record(reason);

    ControlAction a;
    a.kind   = ControlAction::restart;
    a.hard   = r.hard;
    a.busy   = r.unload ? "Unloading" : !model.empty() ? "Loading " + model.substr(model.find_last_of('/') + 1) : "Restarting";
    a.detail = reason;
    post(a);
    // The main loop does it within a few frames; when it does not (it can
    // block too), this thread does.
    const int fallback = r.hard ? kFallbackHardS : kFallbackCleanS;
    spawn([fallback] {
        std::this_thread::sleep_for(std::chrono::seconds(fallback));
        std::printf("ps5lm-app: control: the main loop did not restart in %d s; LoadExec from the control thread\n", fallback);
        const int rc = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
        std::printf("ps5lm-app: control: LoadExec returned 0x%x\n", rc);
    }, 64u << 10);
    return json_reply(202, { { "restarting", true },
                             { "mode", r.hard ? "hard" : "clean" },
                             { "reason", reason },
                             { "next_model", choice.empty() ? json(nullptr) : json(choice) },
                             { "app_args", args_text.empty() ? (drop_args ? json("removed") : json("kept")) : json("written") },
                             { "files_written", written },
                             { "fallback_after_s", fallback },
                             { "boot_id", boot_id_ },
                             { "hint", "the app is replaced; poll GET /ping until boot_id changes, then GET /status "
                                       "until server.verdict is \"ready\" (scripts/ps5lm_ctl.py wait-ready)" } });
}

// ---- the watchdog ---------------------------------------------------------------

void Control::watchdog_loop() {
    double failed_since = -1;
    bool   told = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        WatchdogConfig w;
        bool           tripped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            w       = watchdog_;
            tripped = watchdog_tripped_;
        }
        if (w.watchdog_s == 0 || tripped || restarting_ || steady() - started_ < w.grace_s) {
            failed_since = -1;
            continue;
        }
        const Live live = stats_->snapshot();
        std::string reason;
        if (serving(live.state) && live.metrics_age_s > w.watchdog_s) {
            const Probe h = probe(server_port_, "/health", 2000);
            char text[160];
            std::snprintf(text, sizeof(text), "watchdog: the main loop finished no task for %.0f s (/health %d)",
                          live.metrics_age_s, h.status);
            reason = text;
        }
        if (w.on_failure && live.state == ServerState::failed) {
            failed_since = failed_since < 0 ? steady() : failed_since;
            if (steady() - failed_since > 10) {
                reason = "watchdog: llama-server stopped";
            }
        } else {
            failed_since = -1;
        }
        if (reason.empty()) {
            told = false;
            continue;
        }
        size_t recent = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int64_t hour_ago = (int64_t) std::time(nullptr) - 3600;
            while (!restarts_.empty() && restarts_.front() < hour_ago) {
                restarts_.pop_front();
            }
            recent = restarts_.size();
            if (recent >= w.max_per_hour) {
                watchdog_tripped_ = true;
            }
        }
        if (recent >= w.max_per_hour) {
            if (!told) {
                std::printf("ps5lm-app: control: %s, but %zu restarts in the last hour; the watchdog stops until "
                            "PATCH /watchdog\n", reason.c_str(), recent);
                told = true;
            }
            continue;
        }
        Restart r;
        r.hard   = w.hard;
        r.reason = reason;
        restart(r);
    }
}

// ---- status ---------------------------------------------------------------------

Control::Reply Control::status() {
    const Live  live = stats_->snapshot();
    const Probe h    = probe(server_port_, "/health", 2000);
    const double age = live.metrics_age_s;
    const bool answering = age >= 0 && age < kAnsweringS;
    const bool wedged    = serving(live.state) && age > kWedgedS;
    std::string verdict = state_name(live.state);
    if (restarting_) {
        verdict = "restarting";
    } else if (wedged) {
        verdict = "wedged";
    } else if (serving(live.state) && !answering) {
        verdict = "slow";  // between kAnsweringS and kWedgedS: a long step, or the start of a wedge
    } else if (live.state == ServerState::generating) {
        verdict = "busy";
    }
    WatchdogConfig w;
    json restart = nullptr;
    std::string last;
    size_t recent;
    bool tripped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        w = watchdog_;
        tripped = watchdog_tripped_;
        last = last_restart_;
        const int64_t hour_ago = (int64_t) std::time(nullptr) - 3600;
        recent = (size_t) std::count_if(restarts_.begin(), restarts_.end(), [&](int64_t at) { return at >= hour_ago; });
        if (restarting_) {
            restart = { { "reason", restart_reason_ }, { "since_s", steady() - restart_at_ } };
        }
    }
    json model = nullptr;
    if (!live.model_file.empty() || !live.model_label.empty()) {
        model = { { "label", live.model_label }, { "file", live.model_file },   { "arch", live.model_arch },
                  { "ctx", live.ctx },           { "ctx_used", live.ctx_used }, { "kv_type", live.kv_type },
                  { "kv_gib", live.kv_gib },     { "model_gib", live.model_gib }, { "plan", live.plan },
                  { "preset", live.preset },     { "args", live.args } };
    }
    return json_reply(200, {
        { "app", { { "version", PS5LM_VERSION }, { "boot_id", boot_id_ }, { "started_unix", started_unix_ },
                   { "uptime_s", steady() - started_ }, { "control_port", port_ } } },
        { "server", { { "verdict", verdict },
                      { "state", state_name(live.state) },
                      { "phase", live.phase },
                      { "address", live.address },
                      { "health", { { "status", h.status }, { "ms", h.ms } } },
                      { "main_loop", { { "answering", answering }, { "last_task_s_ago", age < 0 ? json(nullptr) : json(age) } } },
                      { "load_progress", live.load_progress } } },
        { "model", model },
        { "memory_gib", { { "pool", live.pool_gib }, { "free", live.free_gib }, { "cpu_heap", live.heap_gib } } },
        { "load", { { "gpu_busy", live.gpu_busy }, { "cpu_use", live.cpu_use }, { "gen_tps", live.gen_tps },
                    { "prompt_tps", live.prompt_tps }, { "tokens", live.tokens }, { "prompt_tokens", live.prompt_tokens },
                    { "requests", live.requests } } },
        { "console", { { "soc_temp_c", live.soc_temp > -100 ? json(live.soc_temp) : json(nullptr) },
                       { "soc_power_w", live.soc_power_w > 0 ? json(live.soc_power_w) : json(nullptr) } } },
        { "restart", restart },
        { "watchdog", { { "config", watchdog_json(w) }, { "tripped", tripped }, { "restarts_last_hour", recent },
                        { "last_restart", last.empty() ? json(nullptr) : json(last) } } },
    });
}

// ---- routes ---------------------------------------------------------------------

Control::Reply Control::handle(const std::string & method, const std::string & target, const std::string & token,
                               const std::string & body) {
    const size_t qm = target.find('?');
    const std::string route = target.substr(0, qm);
    const auto query = query_of(qm == std::string::npos ? "" : target.substr(qm + 1));

    if (route == "/ping" || route == "/") {
        return json_reply(200, { { "ok", true }, { "app", "ps5lm" }, { "version", PS5LM_VERSION }, { "boot_id", boot_id_ },
                                 { "uptime_s", steady() - started_ }, { "restarting", restarting_.load() } });
    }
    if (!same(token, token_)) {
        return error_reply(401, "missing or wrong token: send \"Authorization: Bearer <token>\" with the token in "
                                "/data/PS5LM/control-token.txt");
    }
    const json j = body.empty() ? json::object() : json::parse(body, nullptr, false);
    const bool get = method == "GET", is_post = method == "POST", put = method == "PUT", patch = method == "PATCH";

    if (route == "/status" && get) {
        return status();
    }
    if (route == "/models" && get) {
        json rows = json::array();
        for (const auto & m : stats_->snapshot().models) {
            rows.push_back({ { "label", m.label }, { "file", m.file }, { "arch", m.arch }, { "size_gib", m.size_gib },
                             { "fits", m.fits }, { "plan", m.plan }, { "current", m.current }, { "drive", m.drive } });
        }
        return json_reply(200, { { "models", rows } });
    }
    if ((route == "/restart" || route == "/load" || route == "/unload") && is_post) {
        if (!j.is_object()) {
            return error_reply(400, "the body is not a JSON object");
        }
        Restart r;
        try {
            r.model      = j.value("model", std::string());
            r.args       = j.value("args", std::vector<std::string>());
            r.clear_args = j.value("clear_args", false);
            r.hard       = j.value("hard", false) || j.value("mode", std::string()) == "hard";
            r.reason     = j.value("reason", std::string());
        } catch (const std::exception & e) {
            return error_reply(400, std::string("bad field: ") + e.what());
        }
        if (route == "/load" && r.model.empty()) {
            return error_reply(400, "/load needs \"model\": a library label, a file name or a path");
        }
        r.unload = route == "/unload";
        if (r.unload && (!r.model.empty() || !r.args.empty())) {
            return error_reply(400, "/unload takes no model or args");
        }
        if (r.reason.empty()) {
            r.reason = "control: " + route.substr(1) + (r.model.empty() ? "" : " " + r.model);
        } else {
            r.reason = "control: " + r.reason;
        }
        return restart(r);
    }
    if (route == "/logs" && get) {
        return json_reply(200, { { "logs", { "llama", "app", "ui", "restarts" } },
                                 { "usage", "GET /logs/<name>?bytes=N (the tail, default 16384, at most 1 MiB) or "
                                            "?since=<offset> (from an offset; X-Log-Size has the next one)" } });
    }
    if (route.rfind("/logs/", 0) == 0 && get) {
        const std::string name = route.substr(6);
        const char * file = name == "llama" ? "llama.log" : name == "app" ? "app.log" : name == "ui" ? "ui.log"
                          : name == "restarts" ? "control-restarts.txt" : nullptr;
        if (file == nullptr) {
            return error_reply(404, "no log \"" + name + "\"; the logs are llama, app, ui and restarts");
        }
        size_t want = kLogDefault;
        if (query.count("bytes")) {
            want = std::min(kLogMax, (size_t) std::strtoull(query.at("bytes").c_str(), nullptr, 10));
        }
        FILE * f = std::fopen(path(file).c_str(), "rb");
        if (!f) {
            return error_reply(404, std::string(file) + " does not exist yet");
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        long from = std::max(0L, size - (long) want);
        if (query.count("since")) {
            from = std::min(size, std::max(0L, std::strtol(query.at("since").c_str(), nullptr, 10)));
            want = std::min(kLogMax, (size_t) (size - from));
        }
        std::fseek(f, from, SEEK_SET);
        Reply r;
        r.type = "text/plain; charset=utf-8";
        r.body.resize((size_t) (size - from) < want ? (size_t) (size - from) : want);
        r.body.resize(std::fread(r.body.data(), 1, r.body.size(), f));
        std::fclose(f);
        r.extra_headers = "X-Log-Size: " + std::to_string(size) + "\r\nX-Log-From: " + std::to_string(from) +
                          "\r\nX-Log-Next: " + std::to_string(from + (long) r.body.size()) + "\r\n";
        return r;
    }
    if (route == "/env" && (get || put || patch)) {
        std::string text;
        read_file(path("env.txt"), &text);
        if (put) {
            text = body;  // the whole file, as text
        } else if (patch) {
            if (!j.is_object()) {
                return error_reply(400, "PATCH /env takes {\"set\": {NAME: value}, \"unset\": [NAME]}");
            }
            std::map<std::string, std::string> set;
            std::vector<std::string> unset;
            try {
                set   = j.value("set", std::map<std::string, std::string>());
                unset = j.value("unset", std::vector<std::string>());
            } catch (const std::exception & e) {
                return error_reply(400, std::string("bad field: ") + e.what());
            }
            for (const auto & [k, v] : set) {
                if (!env_name_ok(k) || v.find('\n') != std::string::npos) {
                    return error_reply(400, "\"" + k + "\" is not a name the app sets (RADV_, MESA_, ACO_, GGML_, LLAMA_), "
                                            "or its value holds a line break");
                }
            }
            // Each line kept, replaced or dropped in place; new names at the end.
            std::string out;
            std::map<std::string, bool> done;
            std::stringstream ss(text);
            for (std::string line; std::getline(ss, line);) {
                const std::string t = trim(line);
                const size_t eq = t.find('=');
                const std::string name = t.empty() || t[0] == '#' || eq == std::string::npos ? "" : t.substr(0, eq);
                if (!name.empty() && std::find(unset.begin(), unset.end(), name) != unset.end()) {
                    continue;
                }
                if (!name.empty() && set.count(name)) {
                    if (!done[name]) {
                        out += name + "=" + set[name] + "\n";
                    }
                    done[name] = true;
                    continue;
                }
                out += line + "\n";
            }
            for (const auto & [k, v] : set) {
                if (!done[k]) {
                    out += k + "=" + v + "\n";
                }
            }
            text = out;
        }
        if (text.size() > 65536) {
            return error_reply(413, "env.txt is limited to 64 KiB");
        }
        if ((put || patch) && !bounded([file = path("env.txt"), text] { write_file(file, text); }, kFileTimeoutMs)) {
            return error_reply(504, "the write to env.txt did not finish in 3 s; /data may be stalled");
        }
        if (put || patch) {
            std::printf("ps5lm-app: control: env.txt written (%zu bytes)\n", text.size());
        }
        return json_reply(200, env_view(text));
    }
    if (route == "/settings" && (get || put || patch)) {
        const std::string file = path("settings.json");
        Settings s = load_settings(file.c_str());
        if (put || patch) {
            const Settings was = s;
            std::string error;
            if (!patch_settings(body, &s, &error)) {
                return error_reply(400, error);
            }
            if (!bounded([file, s] { save_settings(file.c_str(), s); }, kFileTimeoutMs)) {
                return error_reply(504, "the write to settings.json did not finish in 3 s; /data may be stalled");
            }
            std::printf("ps5lm-app: control: settings saved: %s\n", trim(body).c_str());
            ControlAction a;
            a.kind  = ControlAction::settings;
            a.saved = s;
            post(a);
            const bool restart = was.ctx_cap != s.ctx_cap || was.kv_type != s.kv_type || was.introspect != s.introspect ||
                                 was.tools != s.tools;
            json out = json::parse(settings_json(s));
            return json_reply(200, { { "settings", out },
                                     { "restart_needed", restart },
                                     { "note", restart ? "the running server keeps its settings until POST /restart"
                                                       : "applied" } });
        }
        return json_reply(200, { { "settings", json::parse(settings_json(s)) } });
    }
    if (route == "/watchdog" && (get || put || patch)) {
        if (put || patch) {
            std::string error;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                WatchdogConfig w = watchdog_;
                if (!patch_watchdog(j, &w, &error)) {
                    return error_reply(400, error);
                }
                watchdog_         = w;
                watchdog_tripped_ = false;  // any change arms it again
                restarts_.clear();          // and forgets the hour's restarts, across starts too
                rearmed_at_ = (int64_t) std::time(nullptr);
            }
            save_watchdog();
            std::printf("ps5lm-app: control: watchdog set: %s\n", j.dump().c_str());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return json_reply(200, { { "config", watchdog_json(watchdog_) }, { "tripped", watchdog_tripped_ } });
    }
    static const char * routes[] = { "/status", "/models", "/restart", "/load", "/unload", "/logs", "/env", "/settings",
                                     "/watchdog" };
    for (const char * known : routes) {
        if (route == known || (std::string(known) == "/logs" && route.rfind("/logs/", 0) == 0)) {
            return error_reply(405, method + " is not allowed on " + route);
        }
    }
    return error_reply(404, "no route " + route + "; see docs/CONTROL.md");
}

// ---- the socket -----------------------------------------------------------------

namespace {

bool send_all(int fd, const std::string & data) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    for (size_t at = 0; at < data.size();) {
        const ssize_t n = send(fd, data.data() + at, data.size() - at, flags);
        if (n <= 0) {
            return false;
        }
        at += (size_t) n;
    }
    return true;
}

const char * reason_of(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
    }
    return "Error";
}

void respond(int fd, const Control::Reply & r) {
    const std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_of(r.status) +
                             "\r\nContent-Type: " + r.type + "\r\nContent-Length: " + std::to_string(r.body.size()) +
                             "\r\nConnection: close\r\nCache-Control: no-store\r\n" + r.extra_headers + "\r\n";
    send_all(fd, head) && send_all(fd, r.body);
}

std::string lower(std::string s) {
    for (char & c : s) {
        c = (char) std::tolower((unsigned char) c);
    }
    return s;
}

void serve(Control * control, int fd) {
    timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    std::string in;
    char buf[8192];
    size_t head_end = std::string::npos;
    while ((head_end = in.find("\r\n\r\n")) == std::string::npos && in.size() < kMaxHeader) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        in.append(buf, (size_t) n);
    }
    if (head_end == std::string::npos) {
        respond(fd, error_reply(413, "the request's header is too long"));
        return;
    }
    std::stringstream head(in.substr(0, head_end));
    std::string method, target, version, line;
    head >> method >> target >> version;
    std::getline(head, line);
    size_t length = 0;
    std::string token;
    while (std::getline(head, line)) {
        line = trim(line);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
        if (name == "content-length") {
            length = (size_t) std::strtoull(value.c_str(), nullptr, 10);
        } else if (name == "x-ps5lm-token") {
            token = value;
        } else if (name == "authorization" && lower(value.substr(0, 7)) == "bearer ") {
            token = trim(value.substr(7));
        }
    }
    if (length > kMaxBody) {
        respond(fd, error_reply(413, "the body is limited to 1 MiB"));
        return;
    }
    std::string body = in.substr(head_end + 4);
    while (body.size() < length) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        body.append(buf, (size_t) n);
    }
    body.resize(length);
    Control::Reply r;
    try {
        r = control->handle(method, target, token, body);
    } catch (const std::exception & e) {
        r = error_reply(500, std::string("control: ") + e.what());
    }
    respond(fd, r);
}

int listen_on(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in addr = {};
#if defined(__FreeBSD__) || defined(__APPLE__)
    addr.sin_len = sizeof(addr);
#endif
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

void Control::accept_loop() {
    for (int tries = 0;; ++tries) {
        const int fd = listen_on(port_);
        if (fd < 0) {
            if (tries % 30 == 0) {
                std::printf("ps5lm-app: control: cannot listen on %d (errno %d); trying again\n", port_, errno);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        tries = 0;
        for (;;) {
            const int c = accept(fd, nullptr, nullptr);
            if (c < 0) {
                if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) {
                    continue;
                }
                // The network dropped (patches/0005 has the same for 8081): listen again.
                std::printf("ps5lm-app: control: accept failed (errno %d); listening again\n", errno);
                break;
            }
#ifdef SO_NOSIGPIPE
            const int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            if (connections_.fetch_add(1) >= kMaxConnections) {
                connections_.fetch_sub(1);
                respond(c, error_reply(503, "too many control connections are open (a /data read may be stuck)"));
                close(c);
                continue;
            }
            spawn([this, c] {
                serve(this, c);
                close(c);
                connections_.fetch_sub(1);
            });
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace ps5lm
