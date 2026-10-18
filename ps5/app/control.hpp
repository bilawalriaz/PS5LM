// The control port: an HTTP API on its own port (8083) and its own threads,
// for the agents that run experiments against the console and must recover
// from a wedged server with nobody at it.
//
// It never goes through llama-server: a server whose main loop is stuck (a
// full log queue, a task that never returns) still answers /health, while
// /metrics, /slots and every completion hang. The port reads what the stats
// thread already measures (the age of the last /metrics answer is the age of
// the last task the main loop finished), asks /health itself with a short
// timeout, and restarts the app the way the library does: the main loop puts
// the busy screen up and LoadExecs eboot.bin. When the main loop does not get
// there in time (it can block too), the control thread LoadExecs on its own.
//
// Every route but GET /ping needs the token in /data/PS5LM/control-token.txt,
// made at the first start, in an "Authorization: Bearer <token>" or an
// "X-PS5LM-Token" header. See docs/CONTROL.md.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "settings.hpp"
#include "stats.hpp"

namespace ps5lm {

// What the control port asks of the main loop; taken once a frame.
struct ControlAction {
    enum Kind { none, restart, settings } kind = none;
    bool        hard = false;    // restart: LoadExec at once, without llama_server_terminate
    std::string busy, detail;    // restart: the busy screen's two lines
    Settings    saved;           // settings: what settings.json holds now
};

// The watchdog, kept in /data/PS5LM/control.json. Off unless watchdog_s > 0.
struct WatchdogConfig {
    uint32_t watchdog_s   = 0;     // restart when the main loop has not answered for this long
    uint32_t grace_s      = 120;   // nothing in the app's first seconds (the model loads)
    uint32_t max_per_hour = 3;     // then it stops, until PATCH /watchdog arms it again
    bool     hard         = true;  // skip the clean stop: a wedged server does not stop
    bool     on_failure   = false; // also restart when llama-server returned (state "failed")
};

class Control {
  public:
    // `dir` is /data/PS5LM on the console; `server_port` is llama-server's.
    void start(int port, int server_port, const char * dir, StatsCollector * stats);
    bool take(ControlAction * out);  // the main loop, each frame

    // Internal, public for the connection threads.
    struct Reply {
        int         status = 200;
        std::string type   = "application/json";
        std::string body;
        std::string extra_headers;
    };
    Reply handle(const std::string & method, const std::string & target, const std::string & token,
                 const std::string & body);

  private:
    struct Restart {
        std::string model;                 // a path or a library label; empty: the one running
        std::vector<std::string> args;     // app-args.txt, verbatim; empty: leave it as it is
        bool        clear_args = false;    // remove app-args.txt (the planner decides again)
        bool        unload     = false;    // restart with no model
        bool        hard       = false;
        std::string reason;
    };

    void   accept_loop();
    void   watchdog_loop();
    Reply  restart(const Restart & r);
    Reply  status();
    std::string path(const char * name) const;
    void   post(ControlAction a);
    void   record(const std::string & reason);  // the restart history
    void   save_watchdog();

    int              port_ = 8083, server_port_ = 8081;
    std::string      dir_;
    StatsCollector * stats_ = nullptr;
    std::string      token_;
    std::string      boot_id_;
    double           started_ = 0;          // steady seconds
    int64_t          started_unix_ = 0;

    std::mutex                mutex_;       // everything below
    std::deque<ControlAction> actions_;
    WatchdogConfig            watchdog_;
    bool                      watchdog_tripped_ = false;  // max_per_hour reached
    std::string               last_restart_;              // the history's last line, read at start
    std::deque<int64_t>       restarts_;                  // unix times of the history's last hour
    int64_t                   rearmed_at_ = 0;            // unix time of the last PATCH /watchdog
    std::atomic<bool>         restarting_{ false };
    double                    restart_at_ = 0;
    std::string               restart_reason_;
    std::atomic<int>          connections_{ 0 };
};

}  // namespace ps5lm
