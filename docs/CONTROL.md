# The control port: running the console unattended

The app answers on a second port, **8083**, with a small HTTP API meant for scripts and AI agents
that use the console as an inference server and must get it back on their own when it gets stuck:
read the server's state, restart it (same model, another model, other arguments), unload, read the
logs, change `env.txt` and the settings, and an optional watchdog that restarts a wedged server by
itself.

It lives in the app (`ps5/app/control.cpp`), on its own threads and its own socket, not in
llama-server. That is the point: when llama-server's main loop is stuck, `/health` on 8081 still
answers 200 while `/slots`, `/metrics` and every completion hang. Anything that goes through the
server's task queue hangs with them. The control port never does.

> **Status: unbuilt on the console.** The code was written on a Mac and checked there with
> `scripts/control-host-test.sh` (the control port in front of a fake llama-server that can be made
> to wedge; 22 checks). It has not been compiled with the PS5 SDK or run on a console yet. The
> checks to make there are at the end of this page.

## Quick start

```sh
export PS5_HOST=<console-ip>
python3 scripts/ps5lm_ctl.py status        # the verdict, the model, memory, the watchdog
python3 scripts/ps5lm_ctl.py ensure        # ready? if wedged or stopped, restart (hard) and wait
python3 scripts/ps5lm_ctl.py logs llama --bytes 4000
```

`scripts/ps5lm_ctl.py` needs only Python's standard library. It finds the token by itself (below).
From Python:

```python
import sys; sys.path.insert(0, "scripts")
from ps5lm_ctl import Control, Unreachable

ctl = Control("10.101.1.112")
ctl.ensure_ready()                  # before a sweep
try:
    run_sweep()                     # your requests to :8081, each with a timeout
except TimeoutError:
    ctl.ensure_ready()              # wedged: restarted and loaded again, or raises
```

## The token

Every route except `GET /ping` needs the token from `/data/PS5LM/control-token.txt`. The app makes
the file the first time it starts (32 hex digits). Send it as `Authorization: Bearer <token>` or as
`X-PS5LM-Token: <token>`.

```sh
TOKEN=$(curl -s ftp://$PS5_HOST:2121/data/PS5LM/control-token.txt)
curl -s -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/status
```

`ps5lm_ctl.py` takes the token from `PS5_CONTROL_TOKEN`, then from its cache
(`~/.cache/ps5lm/control-token-<host>`), and then fetches it over FTP and caches it. When the cached
token is refused, it fetches it again once. To change the token, write a new one to the file and
restart the app.

The token keeps a stray request (a scanner, a browser tab, the wrong script) from restarting the
console. It is not protection against someone on your network: ftpsrv serves `/data` to anyone
with no password, and that includes the token file.

## Routes

| Route | Does |
|---|---|
| `GET /ping` | No token. `{ok, version, boot_id, uptime_s, restarting}`. `boot_id` changes on every start. |
| `GET /status` | The verdict, server state, `/health`, the main loop's last task, model, memory, load, watchdog. |
| `GET /models` | The library: label, file, fits, the planner's line, current. |
| `POST /restart` | Restart the app and the server. Body (all optional): `model`, `args`, `clear_args`, `hard`, `reason`. |
| `POST /load` | `/restart` with `model` required. |
| `POST /unload` | Restart with no model. |
| `GET /logs/<name>` | `llama`, `app`, `ui`, `restarts`. `?bytes=N` (the tail, default 16 KiB, at most 1 MiB) or `?since=<offset>`. |
| `GET /env`, `PUT /env`, `PATCH /env` | `env.txt`: read it, replace it (text body), or `{"set": {...}, "unset": [...]}`. |
| `GET /settings`, `PATCH /settings` | `settings.json` (the Settings page): read, or change some keys. |
| `GET /watchdog`, `PATCH /watchdog` | The watchdog's settings (`control.json`). |

Restarts answer `202` at once and the app is replaced a few seconds later. Errors are JSON
`{"error": "..."}` with 400 (bad body), 401 (token), 404, 409 (a restart already under way, or a
model that does not fit) or 504 (a write to `/data` did not finish in 3 s).

### Status and the verdict

```sh
curl -s -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/status
```

```json
{
  "app": { "version": "0.10.1", "boot_id": "3f9a0c11d2e4", "started_unix": 1792353000, "uptime_s": 812.4, "control_port": 8083 },
  "server": {
    "verdict": "ready",
    "state": "ready",
    "phase": "",
    "address": "10.101.1.112:8081",
    "health": { "status": 200, "ms": 1.8 },
    "main_loop": { "answering": true, "last_task_s_ago": 0.6 },
    "load_progress": -1.0
  },
  "model": { "label": "MiniCPM5-2B-Q8_0", "file": "/data/PS5LM/models/MiniCPM5-2B-Q8_0.gguf", "ctx": 65536, "ctx_used": 412, "...": "..." },
  "memory_gib": { "pool": 12.0, "free": 7.9, "cpu_heap": 0.4 },
  "load": { "gpu_busy": 0.0, "cpu_use": 0.03, "gen_tps": 0.0, "prompt_tps": 0.0, "tokens": 51234, "prompt_tokens": 902113, "requests": 811 },
  "console": { "soc_temp_c": 61.0, "soc_power_w": 48.2 },
  "restart": null,
  "watchdog": { "config": { "watchdog_s": 0, "...": "..." }, "tripped": false, "restarts_last_hour": 0, "last_restart": "1792352188 control: ensure: wedged" }
}
```

`main_loop.last_task_s_ago` is how long ago `/metrics` last answered. The app's stats thread asks
for it every second, with a one-second timeout. A `/metrics` answer is a task that llama-server's
main loop took from its queue and finished, so this is the age of the last task the main loop
completed. `/health` is answered by the HTTP thread itself and says nothing about the main loop.

| `verdict` | Means | An agent should |
|---|---|---|
| `ready` | a model is loaded and the main loop answered within 5 s | send requests |
| `busy` | the same, while it generates | send requests (one slot: they queue) |
| `slow` | a model is up, the main loop has been silent for 5 to 15 s | wait; a long step, or the start of a wedge |
| `wedged` | a model is up, the main loop has been silent for more than 15 s | `POST /restart` with `"hard": true` |
| `loading` | the model is going onto the GPU (`load_progress` 0..1) | wait |
| `restarting` | a restart was asked and the app has not been replaced yet | wait for a new `boot_id` |
| `no_model` | the app is up with nothing loaded | `POST /load` |
| `failed` | llama-server returned (see `logs llama`) | read the log; `POST /restart` |

`ps5lm_ctl.py ensure` (`Control.ensure_ready()`) follows that table: it waits through `slow`,
`loading` and `restarting`, restarts hard on `wedged` and `failed`, and raises on `no_model`.

### Restarting

```sh
# the same model again; a clean stop first (llama_server_terminate, at most 10 s)
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/restart -d '{}'

# a wedged server: skip the clean stop
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/restart -d '{"hard": true}'

# another model from the library, by label, file name or path
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/load -d '{"model": "MiniCPM5-2B-Q8_0"}'

# your own llama-server arguments (written to app-args.txt; "-m <model>" is added when missing)
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/restart \
     -d '{"args": ["-c", "32768", "-ngl", "99", "--cache-type-k", "q8_0"]}'

# back to the planner's arguments
curl -s -X POST -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/restart -d '{"clear_args": true}'
```

A restart works the way the library's model switch does: the app replaces itself
(`sceSystemServiceLoadExec` on its own `eboot.bin`), and the new process starts from the whole
memory pool and loads what `app-args.txt` or `model.txt` names. That is also why an unload restarts.
Freeing a model in place leaves GPU memory held.

1. The control thread writes the files the next start reads. `model.txt` gets the model you named,
   or the running one, or `none` for an unload. `app-args.txt` is written when you pass `args`, and
   removed for `clear_args`, for an unload and for a named model without `args` (the library does
   the same, because a choice from the library replaces a manual override). Each write gets 3 s. A
   stalled `/data` does not stop the restart, and the answer reports `files_written: false`.
2. It appends `<unix time> <reason>` to `/data/PS5LM/control-restarts.txt` (`GET /logs/restarts`).
3. The main loop shows the busy screen for three frames. A clean restart then stops llama-server
   (`llama_server_terminate`, which waits 10 s at most) and LoadExecs. A hard restart LoadExecs at
   once.
4. **The fallback.** If the process is still there 30 s after a clean request (8 s after a hard
   one), the control thread calls LoadExec itself. That covers a main loop that is blocked too: a
   clean stop that hangs inside `llama_server_terminate`, or a `printf` to a stalled `app.log`.

The answer has the old `boot_id`. Wait for `/ping` to show a different one, then for `/status` to
show `ready`. `ps5lm_ctl.py restart` does this unless you pass `--no-wait`.

A model the planner says does not fit is refused (409) unless you pass `args`. A path outside the
library is refused too (404), because the next start only loads a `model.txt` that names a library
model that fits. Pass `args` to load it anyway.

### Logs

```sh
curl -s -H "Authorization: Bearer $TOKEN" "http://$PS5_HOST:8083/logs/llama?bytes=8192"
curl -s -D - -H "Authorization: Bearer $TOKEN" "http://$PS5_HOST:8083/logs/app?since=0" | head
python3 scripts/ps5lm_ctl.py logs llama --follow
```

Logs come back as `text/plain`. The headers `X-Log-Size`, `X-Log-From` and `X-Log-Next` give the
file's size, the offset of the first byte returned, and the offset to pass as `since` next time.
`restarts` is the restart history. `ui` is the frame-time log.

### env.txt and settings

`env.txt` sets driver, ggml and llama environment variables when the app starts. Only names that
begin with `RADV_`, `MESA_`, `ACO_`, `GGML_` or `LLAMA_` are set, and `LLAMA_ARG_*` are
llama-server's own options. `PATCH` keeps comments and the order of the other lines. A change takes
effect at the next restart. `GET /env` shows, for each name, the file's value and the value the
running process has (`process`).

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/env \
     -d '{"set": {"LLAMA_ARG_LOG_VERBOSITY": "2"}, "unset": ["GGML_VK_DISABLE_F16"]}'
python3 scripts/ps5lm_ctl.py env --set LLAMA_ARG_LOG_VERBOSITY=2
```

`/settings` is the Settings page's file: `auto_load`, `default_model`, `ctx_cap`, `kv_type`,
`sounds`, `tools`, `introspect`, `scratch_limit_gib` and `download_dir`. An unknown key, a key of the
wrong type, or a value out of range is refused, so a typo cannot leave a `settings.json` the app
then fails to read. The dashboard picks the change up at once. `ctx_cap`, `kv_type`, `introspect`
and `tools` reach the server at its next start, and the answer then says `restart_needed: true`.

```sh
# the /v1/introspect endpoint on, then a restart for it to take effect
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/settings -d '{"introspect": true}'
python3 scripts/ps5lm_ctl.py settings --set introspect=true && python3 scripts/ps5lm_ctl.py restart
```

### The watchdog

Off by default. When `watchdog_s` is set, the app restarts by itself in two cases: a model is up
and the main loop has finished no task for `watchdog_s` seconds, or llama-server returned and
`on_failure` is on.

| Key | Default | |
|---|---|---|
| `watchdog_s` | `0` | 0 is off; otherwise at least 15. |
| `grace_s` | `120` | nothing in the app's first seconds |
| `max_per_hour` | `3` | after this many restarts in an hour the watchdog stops (`tripped`) until the next `PATCH /watchdog` |
| `hard` | `true` | skip the clean stop |
| `on_failure` | `false` | also restart 10 s after llama-server returns |

```sh
curl -s -X PATCH -H "Authorization: Bearer $TOKEN" http://$PS5_HOST:8083/watchdog -d '{"watchdog_s": 90}'
python3 scripts/ps5lm_ctl.py watchdog --set watchdog_s=90 max_per_hour=3
```

Choose `watchdog_s` above the longest single step your work makes the main loop take. A
`/v1/introspect` that copies hundreds of megabytes of activations, or a very long prompt, holds the
loop for a while, and `/metrics` waits behind it. The restarts of the last hour are counted from
`control-restarts.txt`, so the limit holds across the watchdog's own restarts. Any
`PATCH /watchdog` arms it again and forgets that hour.

## Files

All of them are in `/data/PS5LM`:

| File | |
|---|---|
| `control-token.txt` | the token, made at the first start |
| `control.json` | the watchdog's settings |
| `control-restarts.txt` | one line per restart: unix time, reason |
| `model.txt`, `app-args.txt` | what the next start loads (as the library and `scripts/ps5lm-app.sh` use them) |

`app.log` has a line for each control action, prefixed `ps5lm-app: control:` or with the restart's
reason.

## How it is built

- `ps5/app/control.cpp` is a plain socket server: one listening thread, and one thread per
  connection with its own 1 MiB stack. When the network drops it listens again, as patch 0005 does
  for 8081. A request that blocks on `/data` holds only its own thread. Past 16 open connections the
  port answers 503 and closes.
- The wedge check costs llama-server nothing new. `StatsCollector` already asks `/metrics` once a
  second, and now also records when that last succeeded (`Live::metrics_age_s`). `/status` asks
  `/health` itself, with a 2 s timeout.
- Nothing on the control port's path writes to `/data` without a 3 s limit, and `/status` reads no
  file at all, so a stalled `/data` (the cause of the wedge that led to this port) cannot hide the
  state.
- The main loop takes the port's actions once a frame (`Control::take`): restarts, and saved
  settings to show on the dashboard.

## Checking it on a PC

```sh
scripts/control-host-test.sh
```

This builds `ps5/app/control_host.cpp` (the control port and the stats thread, with a stand-in for
the frame loop) with the host compiler. It runs that against a fake llama-server whose `/metrics`
can be made to hang while `/health` answers. It then drives `ps5lm_ctl.py` through these cases: the
token, the `wedged` verdict, `ensure`, hard and clean restarts, loads that are refused, `args` and
`clear_args`, the fallback when the main loop is stuck, `env.txt`, settings validation, logs, and
the watchdog restarting a wedged server by itself. Off the console, LoadExec is an exit with code
75, and the script starts the program again.

## To check on the console

These cannot be checked off the console:

1. The app builds with `control.cpp` (`scripts/build-app.sh`). `GET /ping` answers on 8083 from the
   PC, and `app.log` has `control: port 8083`.
2. `control-token.txt` is made at the first start. `/status` with the token gives 200, and without
   it gives 401.
3. `/status` shows `ready` and a `last_task_s_ago` under 2 s with a model loaded and idle. During a
   long generation it shows `busy`, not `slow`.
4. A clean `POST /restart` comes back with the same model and a new `boot_id`.
   `POST /load {"model": ...}` switches models. `POST /unload` comes back with `no_model`.
5. **A hard restart from a real wedge.** Reproduce one with the logging fix (patch 0010) left out
   and a rapid introspection sweep (`/metrics` hangs, `/health` stays 200). Check that `/status`
   says `wedged` after 15 s and that `{"hard": true}` brings the server back without anyone at the
   console.
6. **LoadExec from the control thread.** Every LoadExec so far came from the main thread. Restart
   with the main loop blocked (for example a debug build that sleeps in the loop after taking the
   action) and check that the fallback relaunches the app after 8 s. Check also that the console
   stays healthy afterwards: no lost video, and the next launch is normal.
7. LoadExec while a thread of llama-server is blocked in a write to `/data`: the process is
   replaced, and does not hang in "Kill for LoadExec".
8. The watchdog with `watchdog_s: 60` restarts a reproduced wedge once and stops after
   `max_per_hour`.
9. `PATCH /settings {"introspect": true}` and a restart turn `/v1/introspect` on. `PATCH /env` and a
   restart show the variable under `process` in `GET /env`.
10. The dashboard stays smooth while an agent polls `/status` once a second (`ui.log` shows no new
    slow frames).
