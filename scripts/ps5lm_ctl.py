#!/usr/bin/env python3
"""Client for the PS5LM app's control port (8083): status, restarts, logs, settings.

    export PS5_HOST=10.101.1.112
    python3 scripts/ps5lm_ctl.py status            the verdict: ready, busy, slow, wedged, loading, ...
    python3 scripts/ps5lm_ctl.py ensure            ready, or recover (restart, hard) and wait until it is
    python3 scripts/ps5lm_ctl.py restart [--model M] [--hard] [--no-wait]
    python3 scripts/ps5lm_ctl.py load MiniCPM5-2B-Q8_0
    python3 scripts/ps5lm_ctl.py unload
    python3 scripts/ps5lm_ctl.py wait-ready [--timeout 300]
    python3 scripts/ps5lm_ctl.py logs llama [--bytes 8192] [--follow]
    python3 scripts/ps5lm_ctl.py env [--set NAME=value ...] [--unset NAME ...]
    python3 scripts/ps5lm_ctl.py settings [--set introspect=true ctx_cap=65536]
    python3 scripts/ps5lm_ctl.py watchdog [--set watchdog_s=90 max_per_hour=3]
    python3 scripts/ps5lm_ctl.py models

The token is PS5_CONTROL_TOKEN, else the copy cached in ~/.cache/ps5lm, else
/data/PS5LM/control-token.txt fetched over the console's FTP server (2121) and
cached. Every command prints JSON (logs print text) and exits 0 on success, 1
on an error, 2 when the console did not answer or did not become ready.

No dependencies outside the standard library; import it and use Control from a
notebook or an agent's harness:

    from ps5lm_ctl import Control
    ctl = Control("10.101.1.112")
    ctl.ensure_ready()           # call before a sweep, and after a request times out
See docs/CONTROL.md.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


class ControlError(RuntimeError):
    def __init__(self, message: str, status: int = 0, body: object = None):
        super().__init__(message)
        self.status = status
        self.body = body


class Unreachable(ControlError):
    """Nothing answered on the control port (the app is restarting, or not running)."""


class Control:
    def __init__(self, host: str | None = None, port: int | None = None, token: str | None = None,
                 timeout: float = 15.0, ftp_port: int | None = None):
        self.host = host or os.environ.get("PS5_HOST") or os.environ.get("PS5LM_HOST")
        if not self.host:
            raise ControlError("set PS5_HOST to the console's address, or pass --host")
        self.port = port or int(os.environ.get("PS5LM_CONTROL_PORT", "8083"))
        self.ftp_port = ftp_port or int(os.environ.get("PS5_FTP_PORT", "2121"))
        self.timeout = timeout
        self._token = token or os.environ.get("PS5_CONTROL_TOKEN") or os.environ.get("PS5LM_CONTROL_TOKEN")
        self._token_from_env = self._token is not None

    # -- the token --------------------------------------------------------

    def _cache(self) -> Path:
        return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "ps5lm" / f"control-token-{self.host}"

    def token(self, refresh: bool = False) -> str:
        if self._token and not refresh:
            return self._token
        cache = self._cache()
        if not refresh and cache.exists():
            self._token = cache.read_text().strip()
            return self._token
        path = os.environ.get("PS5LM_TOKEN_PATH", "/data/PS5LM/control-token.txt")
        url = f"ftp://{self.host}:{self.ftp_port}{path}"
        try:
            with urllib.request.urlopen(url, timeout=self.timeout) as r:
                self._token = r.read().decode().strip().splitlines()[0].strip()
        except Exception as e:  # noqa: BLE001
            raise ControlError(f"no token: set PS5_CONTROL_TOKEN, or let {url} answer ({e})") from e
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(self._token + "\n")
        cache.chmod(0o600)
        return self._token

    # -- plumbing ---------------------------------------------------------

    def request(self, method: str, path: str, body: object = None, *, auth: bool = True,
                timeout: float | None = None, raw: bool = False):
        """(status, headers, data): data is the parsed JSON, or bytes with raw=True."""
        for attempt in (0, 1):
            data = None
            headers = {}
            if isinstance(body, (bytes, str)):
                data = body.encode() if isinstance(body, str) else body
                headers["Content-Type"] = "text/plain"
            elif body is not None:
                data = json.dumps(body).encode()
                headers["Content-Type"] = "application/json"
            if auth:
                headers["Authorization"] = "Bearer " + self.token(refresh=attempt == 1)
            req = urllib.request.Request(f"http://{self.host}:{self.port}{path}", data=data, method=method,
                                         headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                    status, hdrs, payload = r.status, dict(r.headers), r.read()
            except urllib.error.HTTPError as e:
                status, hdrs, payload = e.code, dict(e.headers), e.read()
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                raise Unreachable(f"{self.host}:{self.port} did not answer: {e}") from e
            # A token cached before the console made a new one: fetch it again, once.
            if status == 401 and auth and attempt == 0 and not self._token_from_env:
                continue
            break
        if raw:
            return status, hdrs, payload
        try:
            parsed = json.loads(payload) if payload else None
        except ValueError:
            parsed = payload.decode(errors="replace")
        return status, hdrs, parsed

    def _call(self, method: str, path: str, body: object = None, ok=(200,), **kw):
        status, _, data = self.request(method, path, body, **kw)
        if status not in ok:
            message = data.get("error") if isinstance(data, dict) else data
            raise ControlError(f"{method} {path}: {status} {message}", status, data)
        return data

    # -- the API ----------------------------------------------------------

    def ping(self, timeout: float = 3.0) -> dict:
        return self._call("GET", "/ping", auth=False, timeout=timeout)

    def status(self) -> dict:
        return self._call("GET", "/status")

    def models(self) -> list:
        return self._call("GET", "/models")["models"]

    def restart(self, model: str | None = None, args: list[str] | None = None, hard: bool = False,
                clear_args: bool = False, reason: str | None = None) -> dict:
        body: dict = {"hard": hard}
        if model:
            body["model"] = model
        if args:
            body["args"] = args
        if clear_args:
            body["clear_args"] = True
        if reason:
            body["reason"] = reason
        return self._call("POST", "/restart", body, ok=(202,))

    def load(self, model: str, args: list[str] | None = None, hard: bool = False) -> dict:
        body: dict = {"model": model, "hard": hard}
        if args:
            body["args"] = args
        return self._call("POST", "/load", body, ok=(202,))

    def unload(self, hard: bool = False) -> dict:
        return self._call("POST", "/unload", {"hard": hard}, ok=(202,))

    def logs(self, name: str = "llama", nbytes: int | None = None, since: int | None = None) -> tuple[str, int]:
        """(text, the offset to pass as `since` next time)."""
        q = []
        if nbytes is not None:
            q.append(f"bytes={nbytes}")
        if since is not None:
            q.append(f"since={since}")
        path = f"/logs/{name}" + ("?" + "&".join(q) if q else "")
        status, headers, data = self.request("GET", path, raw=True)
        if status != 200:
            raise ControlError(f"GET {path}: {status} {data.decode(errors='replace').strip()}", status)
        nxt = int({k.lower(): v for k, v in headers.items()}.get("x-log-next", "0"))
        return data.decode(errors="replace"), nxt

    def env(self, set: dict | None = None, unset: list[str] | None = None) -> dict:  # noqa: A002
        if not set and not unset:
            return self._call("GET", "/env")
        return self._call("PATCH", "/env", {"set": set or {}, "unset": unset or []})

    def settings(self, **changes) -> dict:
        return self._call("PATCH", "/settings", changes) if changes else self._call("GET", "/settings")

    def watchdog(self, **changes) -> dict:
        return self._call("PATCH", "/watchdog", changes) if changes else self._call("GET", "/watchdog")

    # -- waiting and recovering -------------------------------------------

    def wait_ready(self, old_boot: str | None = None, timeout: float = 300.0, want: str = "ready",
                   interval: float = 2.0, log=None) -> dict:
        """Waits for a new app (when old_boot is given), then for the server.

        want="ready": the model answers (verdict ready or busy, /health 200).
        want="no_model": the app is up with nothing loaded (after an unload).
        Raises Unreachable on timeout, ControlError when the server failed.
        """
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                p = self.ping()
                if old_boot and p.get("boot_id") == old_boot:
                    raise Unreachable("the old app is still up")
                s = self.status()
            except Unreachable as e:
                last = str(e)
                time.sleep(interval)
                continue
            v = s["server"]["verdict"]
            if v != last and log:
                log(f"verdict {v}" + (f", load {s['server']['load_progress']:.0%}" if v == "loading"
                                      and s["server"]["load_progress"] >= 0 else ""))
            last = v
            if want == "ready" and v in ("ready", "busy") and s["server"]["health"]["status"] == 200:
                return s
            if want == "no_model" and v == "no_model":
                return s
            if v == "failed":
                raise ControlError("llama-server stopped; see logs llama", body=s)
            if v == "no_model" and want == "ready":
                raise ControlError("the app came up with no model loaded", body=s)
            time.sleep(interval)
        raise Unreachable(f"not {want} after {timeout:.0f} s (last: {last})")

    def ensure_ready(self, timeout: float = 300.0, log=None) -> dict:
        """Returns the status once the server answers; restarts it (hard) when
        it is wedged or stopped. Call it before a sweep and after a timeout."""
        try:
            s = self.status()
        except Unreachable:
            return self.wait_ready(timeout=timeout, log=log)  # restarting, most likely
        v = s["server"]["verdict"]
        # "slow" turns into "wedged" after 15 s without a task, or comes back.
        for _ in range(10):
            if v != "slow":
                break
            time.sleep(2)
            s = self.status()
            v = s["server"]["verdict"]
        if v in ("ready", "busy"):
            return s
        if v in ("loading", "restarting", "slow"):
            return self.wait_ready(old_boot=s["app"]["boot_id"] if v == "restarting" else None,
                                   timeout=timeout, log=log)
        if v == "no_model":
            raise ControlError("no model is loaded; load one with: load <model>", body=s)
        if log:
            log(f"verdict {v}: restarting, hard")
        r = self.restart(hard=True, reason=f"ensure: {v}")
        return self.wait_ready(old_boot=r["boot_id"], timeout=timeout, log=log)


def _kv(pairs: list[str]) -> dict:
    out = {}
    for p in pairs:
        k, sep, v = p.partition("=")
        if not sep:
            raise SystemExit(f"{p}: expected NAME=value")
        try:
            out[k] = json.loads(v)  # true, 65536, "text"
        except ValueError:
            out[k] = v
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", help="the console (default: $PS5_HOST)")
    ap.add_argument("--port", type=int, help="the control port (default 8083)")
    ap.add_argument("--token", help="the control token (default: $PS5_CONTROL_TOKEN, the cache, or FTP)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    sub.add_parser("status")
    sub.add_parser("models")
    for name in ("restart", "load"):
        p = sub.add_parser(name)
        if name == "load":
            p.add_argument("model")
        else:
            p.add_argument("--model")
            p.add_argument("--clear-args", action="store_true", help="remove app-args.txt")
        p.add_argument("--arg", action="append", dest="args", help="an app-args.txt line (repeat)")
        p.add_argument("--hard", action="store_true", help="skip the clean stop")
        p.add_argument("--no-wait", action="store_true")
        p.add_argument("--timeout", type=float, default=300)
    p = sub.add_parser("unload")
    p.add_argument("--hard", action="store_true")
    p.add_argument("--no-wait", action="store_true")
    p.add_argument("--timeout", type=float, default=300)
    p = sub.add_parser("wait-ready")
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--boot", help="wait for an app other than this boot_id first")
    p = sub.add_parser("ensure")
    p.add_argument("--timeout", type=float, default=300)
    p = sub.add_parser("logs")
    p.add_argument("name", nargs="?", default="llama", choices=["llama", "app", "ui", "restarts"])
    p.add_argument("--bytes", type=int, default=16384)
    p.add_argument("--follow", "-f", action="store_true")
    p = sub.add_parser("env")
    p.add_argument("--set", nargs="+", default=[], metavar="NAME=value")
    p.add_argument("--unset", nargs="+", default=[], metavar="NAME")
    for name in ("settings", "watchdog"):
        p = sub.add_parser(name)
        p.add_argument("--set", nargs="+", default=[], metavar="key=value")
    a = ap.parse_args(argv)

    def log(msg: str) -> None:
        print(f"ps5lm-ctl: {msg}", file=sys.stderr, flush=True)

    def show(obj) -> None:
        print(json.dumps(obj, indent=2))

    try:
        ctl = Control(a.host, a.port, a.token)
        if a.cmd == "ping":
            show(ctl.ping())
        elif a.cmd == "status":
            show(ctl.status())
        elif a.cmd == "models":
            show(ctl.models())
        elif a.cmd in ("restart", "load", "unload"):
            if a.cmd == "restart":
                r = ctl.restart(a.model, a.args, a.hard, a.clear_args)
            elif a.cmd == "load":
                r = ctl.load(a.model, a.args, a.hard)
            else:
                r = ctl.unload(a.hard)
            log(f"restarting ({r['mode']}), next model {r['next_model']}")
            show(r if a.no_wait else ctl.wait_ready(r["boot_id"], a.timeout,
                                                    "no_model" if a.cmd == "unload" else "ready", log=log))
        elif a.cmd == "wait-ready":
            show(ctl.wait_ready(a.boot, a.timeout, log=log))
        elif a.cmd == "ensure":
            show(ctl.ensure_ready(a.timeout, log=log))
        elif a.cmd == "logs":
            text, nxt = ctl.logs(a.name, a.bytes)
            sys.stdout.write(text)
            while a.follow:
                time.sleep(1)
                text, nxt = ctl.logs(a.name, since=nxt)
                sys.stdout.write(text)
                sys.stdout.flush()
        elif a.cmd == "env":
            sets = {k: str(v).lower() if isinstance(v, bool) else str(v) for k, v in _kv(a.set).items()}
            show(ctl.env(sets, a.unset))
        elif a.cmd == "settings":
            show(ctl.settings(**_kv(a.set)))
        elif a.cmd == "watchdog":
            show(ctl.watchdog(**_kv(a.set)))
    except Unreachable as e:
        log(str(e))
        return 2
    except ControlError as e:
        log(str(e))
        if e.body is not None and not isinstance(e.body, str):
            show(e.body)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
