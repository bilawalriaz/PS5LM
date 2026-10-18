#!/usr/bin/env python3
"""Client for llama-inspect, the PS5LM build of llama.cpp with its model opened up.

    python3 scripts/ps5lm_inspect.py info    --host 10.101.1.112
    python3 scripts/ps5lm_inspect.py tokens  --host 10.101.1.112 --text "Hello"
    python3 scripts/ps5lm_inspect.py tensors --host 10.101.1.112 --prompt "Hello"
    python3 scripts/ps5lm_inspect.py forward --host 10.101.1.112 --prompt "The capital of France is" --top-k 5
    python3 scripts/ps5lm_inspect.py capture --host 10.101.1.112 --prompt "The capital of France is" \\
            --tensors 'ffn_gate-\\d+' --top-k 8

The library has no dependencies outside the standard library; import it from a
notebook or another script with InspectClient.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
import urllib.error
import urllib.request


class InspectError(RuntimeError):
    pass


class InspectClient:
    """One HTTP client for one llama-inspect process."""

    def __init__(self, host: str = "127.0.0.1", port: int = 8090, timeout: float = 600.0):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout

    # -- plumbing ---------------------------------------------------------

    def _request(self, path: str, payload: dict | None = None) -> dict:
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(
            self.base + path,
            data=data,
            headers={"Content-Type": "application/json"} if data else {},
            method="POST" if data is not None else "GET",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as res:
                body = res.read()
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            try:
                detail = json.loads(detail)["error"]
            except Exception:
                pass
            raise InspectError(f"{e.code}: {detail}") from None
        except urllib.error.URLError as e:
            raise InspectError(f"cannot reach {self.base}: {e.reason}") from None
        return json.loads(body)

    # -- endpoints --------------------------------------------------------

    def info(self) -> dict:
        return self._request("/v1/inspect/info")

    def health(self) -> dict:
        return self._request("/health")

    def tokenize(self, text: str, add_special: bool = True, parse_special: bool = True) -> dict:
        return self._request("/v1/inspect/tokenize", {
            "text": text, "add_special": add_special, "parse_special": parse_special,
        })

    def forward(
        self,
        prompt: str | None = None,
        tokens: list[int] | None = None,
        *,
        logits: str = "last",
        indices: list[int] | None = None,
        top_k: int = 20,
        raw_logits: bool = False,
        capture: dict | None = None,
        interventions: list[dict] | None = None,
        baseline: bool = False,
        add_special: bool = True,
        parse_special: bool = True,
    ) -> dict:
        body: dict = {
            "logits": {"positions": logits, "top_k": top_k, "raw": raw_logits},
            "baseline": baseline,
            "add_special": add_special,
            "parse_special": parse_special,
        }
        if prompt is not None:
            body["prompt"] = prompt
        if tokens is not None:
            body["tokens"] = tokens
        if indices is not None:
            body["logits"]["indices"] = indices
        if capture is not None:
            body["capture"] = capture
        if interventions is not None:
            body["interventions"] = interventions
        return self._request("/v1/inspect/forward", body)

    def generate(
        self,
        prompt: str,
        *,
        max_tokens: int = 32,
        temperature: float = 0.0,
        top_k: int = 40,
        logits_top_k: int = 10,
        capture: dict | None = None,
        capture_each: bool = True,
        seed: int = 0,
    ) -> dict:
        body = {
            "prompt": prompt,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "top_k": top_k,
            "logits_top_k": logits_top_k,
            "capture_each": capture_each,
            "seed": seed,
        }
        if capture is not None:
            body["capture"] = capture
        return self._request("/v1/inspect/generate", body)

    def tensors(self, prompt: str = "Hello") -> dict:
        return self._request("/v1/inspect/tensors", {"prompt": prompt})


# -- decoding helpers ------------------------------------------------------


def decode_f32(entry: dict) -> list[float]:
    """Turn a raw activation or logit entry into floats."""
    if entry.get("encoding") != "f32le-base64":
        raise ValueError("entry is not f32le-base64")
    raw = base64.b64decode(entry["data"])
    return list(struct.unpack(f"<{len(raw) // 4}f", raw))


def top_by_logit(logits_entry: dict) -> list[dict]:
    return logits_entry["top"]


def activations_by_name(result: dict) -> dict[str, dict]:
    return {a["name"]: a for a in result.get("activations", [])}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="talk to llama-inspect")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("info")
    sub.add_parser("health")

    t = sub.add_parser("tokens")
    t.add_argument("--text", required=True)

    ts = sub.add_parser("tensors")
    ts.add_argument("--prompt", default="Hello")

    f = sub.add_parser("forward")
    f.add_argument("--prompt", required=True)
    f.add_argument("--top-k", type=int, default=10)
    f.add_argument("--all-positions", action="store_true")
    f.add_argument("--raw-logits", action="store_true")

    c = sub.add_parser("capture")
    c.add_argument("--prompt", required=True)
    c.add_argument("--tensors", action="append", required=True,
                   help="regex over graph tensor names, e.g. 'ffn_gate-\\\\d+'; repeatable")
    c.add_argument("--mode", choices=["summary", "raw"], default="summary")
    c.add_argument("--top-k", type=int, default=8)
    c.add_argument("--positions", type=int, nargs="*", default=None)

    args = ap.parse_args(argv)
    client = InspectClient(args.host, args.port)

    try:
        if args.command == "info":
            out = client.info()
        elif args.command == "health":
            out = client.health()
        elif args.command == "tokens":
            out = client.tokenize(args.text)
        elif args.command == "tensors":
            out = client.tensors(args.prompt)
        elif args.command == "forward":
            out = client.forward(
                args.prompt,
                logits="all" if args.all_positions else "last",
                top_k=args.top_k,
                raw_logits=args.raw_logits,
            )
        elif args.command == "capture":
            capture = {
                "tensors": args.tensors,
                "mode": args.mode,
                "top_k": args.top_k,
            }
            if args.positions:
                capture["positions"] = args.positions
            out = client.forward(args.prompt, capture=capture, top_k=args.top_k)
        else:
            raise AssertionError(args.command)
    except InspectError as e:
        print(f"inspect: {e}", file=sys.stderr)
        return 1

    print(json.dumps(out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
