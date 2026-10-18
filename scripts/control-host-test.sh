#!/usr/bin/env bash
# The control port (ps5/app/control.cpp) on this PC, against a fake
# llama-server that can be made to wedge the way the console's did: /health
# answers, /metrics never does. Builds ps5/app/control_host.cpp with the host
# compiler and walks scripts/ps5lm_ctl.py through status, the wedge verdict,
# a hard and a clean restart, the fallback when the main loop is stuck, the
# watchdog, env.txt, settings and logs.
#
#   scripts/control-host-test.sh          needs c++ (C++20) and python3
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
llama="${LLAMA_DIR:-$root/third_party/llama.cpp}"
out="$root/build/control-host"
mkdir -p "$out"
${HOST_CXX:-c++} -std=c++20 -O1 -Wall -Wextra -I "$llama/vendor" -I "$root/ps5/app" \
    "$root/ps5/app/control_host.cpp" "$root/ps5/app/control.cpp" "$root/ps5/app/settings.cpp" \
    "$root/ps5/app/stats.cpp" -pthread -o "$out/control_host"

dir=$(mktemp -d)
cport=${CONTROL_PORT:-18083}
sport=${SERVER_PORT:-18081}
pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null || true; wait 2>/dev/null || true; pkill -f "$out/control_host" 2>/dev/null || true; rm -rf "$dir"; }
trap cleanup EXIT
mkdir -p "$dir/models"
echo "llama.log line 1" > "$dir/llama.log"

# A llama-server that answers /health always and /metrics unless $dir/wedge exists.
python3 - "$dir" "$sport" <<'EOF' &
import os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
d, port = sys.argv[1], int(sys.argv[2])
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/metrics" and os.path.exists(os.path.join(d, "wedge")):
            time.sleep(30); return
        body = {"/health": b'{"status":"ok"}', "/slots": b'[{"n_decoded":0}]',
                "/metrics": b"# x\nllamacpp:tokens_predicted_total 1\nllamacpp:requests_processing 0\n"}.get(self.path)
        self.send_response(200 if body else 404); self.end_headers(); self.wfile.write(body or b"")
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
EOF
pids+=($!)

# The app: started again whenever it "LoadExecs" (exit 75), as the console would.
( while true; do
    rc=0; "$out/control_host" "$dir" "$cport" "$sport" >> "$dir/app.log" 2>&1 || rc=$?
    [ "$rc" = 75 ] || exit "$rc"
  done ) &
pids+=($!)

export PS5_HOST=127.0.0.1 PS5LM_CONTROL_PORT=$cport XDG_CACHE_HOME="$dir/cache"
ctl() { python3 "$root/scripts/ps5lm_ctl.py" "$@"; }
pass=0
check() {  # check <what> <command...>: the command must succeed
    local what=$1; shift
    if "$@" > "$dir/out" 2>&1; then pass=$((pass + 1)); echo "ok   $what"; else
        echo "FAIL $what"; cat "$dir/out"; echo "--- app.log"; tail -20 "$dir/app.log"; exit 1; fi
}
verdict() { ctl status | python3 -c 'import json,sys; print(json.load(sys.stdin)["server"]["verdict"])'; }
until [ -f "$dir/control-token.txt" ]; do sleep 0.2; done
export PS5_CONTROL_TOKEN=$(head -1 "$dir/control-token.txt")

check "ping needs no token" env -u PS5_CONTROL_TOKEN python3 "$root/scripts/ps5lm_ctl.py" --token wrong ping
check "a wrong token is refused" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' --token wrong status"
check "ready once /metrics answers" ctl wait-ready --timeout 20
touch "$dir/wedge"
sleep 17
check "/health up, /metrics silent 15 s: wedged" test "$(verdict)" = wedged
check "ensure restarts a wedged server (hard) and waits" sh -c "( sleep 4; rm -f '$dir/wedge' ) & python3 '$root/scripts/ps5lm_ctl.py' ensure --timeout 30"
check "the restart is in the history" sh -c "python3 '$root/scripts/ps5lm_ctl.py' logs restarts | grep -q 'control: ensure: wedged'"
check "model.txt names the running model" grep -q Small-Q8_0 "$dir/model.txt"
check "load refuses a model that does not fit" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' load Huge-F16 --no-wait"
check "load refuses an unknown model" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' load Nope --no-wait"
check "load by label, clean, waits for the new app" ctl load Small-Q8_0 --timeout 30
check "args go to app-args.txt with -m" sh -c "python3 '$root/scripts/ps5lm_ctl.py' restart --arg=-c --arg=8192 --no-wait >/dev/null && sleep 3 && grep -qx -- -m '$dir/app-args.txt' && grep -qx 8192 '$dir/app-args.txt'"
check "clear-args removes app-args.txt" sh -c "python3 '$root/scripts/ps5lm_ctl.py' wait-ready --timeout 30 >/dev/null && python3 '$root/scripts/ps5lm_ctl.py' restart --clear-args --timeout 30 >/dev/null && test ! -e '$dir/app-args.txt'"
# The main loop stuck: the control thread LoadExecs by itself after 8 s.
touch "$dir/stuck"
check "the next app comes up with its main loop stuck" ctl restart --timeout 30
check "fallback LoadExec when the main loop is stuck" sh -c "
    rm -f '$dir/stuck'
    python3 '$root/scripts/ps5lm_ctl.py' restart --hard --timeout 30 >/dev/null && grep -q 'did not restart in 8 s' '$dir/app.log'"
check "env: set and unset keep the other lines" sh -c "
    printf '# note\nLLAMA_ARG_LOG_VERBOSITY=3\nGGML_VK_X=1\n' > '$dir/env.txt'
    python3 '$root/scripts/ps5lm_ctl.py' env --set LLAMA_ARG_LOG_VERBOSITY=2 RADV_DEBUG=nocache --unset GGML_VK_X >/dev/null
    test \"\$(cat '$dir/env.txt')\" = \"\$(printf '# note\nLLAMA_ARG_LOG_VERBOSITY=2\nRADV_DEBUG=nocache')\""
check "env: a name the app ignores is refused" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' env --set PATH=/x"
check "settings: introspect on, says a restart is needed" sh -c "python3 '$root/scripts/ps5lm_ctl.py' settings --set introspect=true | grep -q '\"restart_needed\": true' && grep -q '\"introspect\": true' '$dir/settings.json'"
check "settings: a wrong type is refused" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' settings --set ctx_cap='\"64k\"'"
check "logs: the tail" sh -c "python3 '$root/scripts/ps5lm_ctl.py' logs llama | grep -q 'line 1'"
check "watchdog: below 15 s is refused" sh -c "! python3 '$root/scripts/ps5lm_ctl.py' watchdog --set watchdog_s=5"
check "watchdog: on, and saved" sh -c "python3 '$root/scripts/ps5lm_ctl.py' watchdog --set watchdog_s=15 grace_s=0 max_per_hour=2 >/dev/null && grep -q '\"watchdog_s\": 15' '$dir/control.json'"
boot=$(ctl ping | python3 -c 'import json,sys; print(json.load(sys.stdin)["boot_id"])')
touch "$dir/wedge"
check "watchdog restarts a wedged server by itself" sh -c "
    for i in \$(seq 1 40); do sleep 1; b=\$(python3 '$root/scripts/ps5lm_ctl.py' ping 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"boot_id\"])' 2>/dev/null || true)
      [ -n \"\$b\" ] && [ \"\$b\" != $boot ] && grep -q 'watchdog: the main loop finished no task' '$dir/control-restarts.txt' && exit 0; done; exit 1"
rm -f "$dir/wedge"
echo "control-host-test: $pass checks passed"
