#!/usr/bin/env python3
"""Put this build on the console and exercise the introspection endpoint there.

    python3 scripts/introspect-console-test.py --host 192.168.1.50 \
        --title build/app-ps5/title/PPSA99575 \
        --model /data/PS5LM/models/MiniCPM5-2B-Q8_0.gguf

Steps, each printed as it happens:

1. upload the title folder to /data/homebrew/<id> (FTP 2121),
2. write /data/PS5LM/launch.txt and send build/launch/launch.elf to elfldr (9021),
3. wait for the server on 8081 and for the model to be loaded (llama.log),
4. probe it: a chat completion, then /v1/introspect for logits, a full
   vocabulary row, activations, every prompt position, a chat-template prompt,
   and one intervention, printing what came back and how long it took.

Nothing is written outside /data/homebrew/<id> and /data/PS5LM/. The numbers it
prints are the ones to record; --json-out keeps them.
"""

from __future__ import annotations

import argparse
import base64
import ftplib
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request

FTP_HOST_PORT = 2121
ELF_PORT = 9021
HTTP_PORT = 8081


def log(msg: str) -> None:
    print(msg, flush=True)


class Console:
    def __init__(self, host: str, user: str, password: str):
        self.host = host
        self.ftp = ftplib.FTP(timeout=60)
        self.ftp.connect(host, FTP_HOST_PORT)
        self.ftp.login(user, password)

    def upload(self, local: str, remote: str) -> None:
        directories = remote.split("/")[1:-1]
        path = ""
        for d in directories:
            path += "/" + d
            try:
                self.ftp.mkd(path)
            except ftplib.error_perm:
                pass
        with open(local, "rb") as f:
            self.ftp.storbinary(f"STOR {remote}", f)

    def write(self, remote: str, text: str) -> None:
        import io

        self.ftp.storbinary(f"STOR {remote}", io.BytesIO(text.encode()))

    def read(self, remote: str, limit: int = 400_000) -> str:
        chunks: list[bytes] = []
        total = 0
        try:
            self.ftp.retrbinary(f"RETR {remote}", lambda b: chunks.append(b))
        except ftplib.error_perm as e:
            return f"<{e}>"
        data = b"".join(chunks)
        return data[-limit:].decode("utf-8", "replace")

    def deploy_title(self, title_dir: str, title_id: str) -> int:
        count = 0
        for root, _dirs, files in os.walk(title_dir):
            for name in files:
                local = os.path.join(root, name)
                rel = os.path.relpath(local, title_dir)
                self.upload(local, f"/data/homebrew/{title_id}/{rel}")
                count += 1
        return count

    def send_elf(self, path: str) -> None:
        with open(path, "rb") as f:
            data = f.read()
        with socket.create_connection((self.host, ELF_PORT), timeout=30) as s:
            s.sendall(data)
        log(f"  sent {os.path.basename(path)} ({len(data)} bytes) to port {ELF_PORT}")


def http(host: str, path: str, body: dict | None = None, timeout: float = 600.0):
    url = f"http://{host}:{HTTP_PORT}{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as e:
        raw = e.read()
        return time.time() - t0, json.loads(raw)
    except Exception as e:  # connection refused while the app boots
        return time.time() - t0, {"error": {"message": str(e)}}
    try:
        return time.time() - t0, json.loads(raw)
    except json.JSONDecodeError:
        return time.time() - t0, {"error": {"message": f"non-JSON answer, {len(raw)} bytes"}}


def wait_for_health(host: str, seconds: int) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        _, d = http(host, "/health", timeout=5)
        if d.get("status") == "ok":
            return True
        time.sleep(5)
    return False


def top_of(doc: dict, index: int | None = None):
    it = doc.get("introspection") or {}
    entries = it.get("positions") or []
    if not entries:
        return None
    if index is not None:
        entries = [e for e in entries if e["index"] == index] or entries[-1:]
    e = entries[-1]
    return e, (e.get("logits") or {})


def kl(a: list[dict], b: list[dict]) -> float:
    """KL(a||b) over the union of two top-k candidate lists, in nats (a rough divergence)."""
    import math

    pa = {t["id"]: t.get("prob", 0.0) for t in a}
    pb = {t["id"]: t.get("prob", 0.0) for t in b}
    total = 0.0
    for tid, p in pa.items():
        q = pb.get(tid, 1e-9)
        if p > 0:
            total += p * math.log(p / q)
    return total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--title", default="build/app-ps5/title/PPSA99575")
    ap.add_argument("--model", default="/data/PS5LM/models/MiniCPM5-2B-Q8_0.gguf")
    ap.add_argument("--launcher", default="build/launch/launch.elf")
    ap.add_argument("--user", default="anonymous")
    ap.add_argument("--password", default="anonymous")
    ap.add_argument("--title-id", default="PPSA99575")
    ap.add_argument("--prompt", default="Once upon a time, in a small village,")
    ap.add_argument("--skip-deploy", action="store_true")
    ap.add_argument("--skip-launch", action="store_true")
    ap.add_argument("--wait", type=int, default=300, help="seconds to wait for the model")
    ap.add_argument("--json-out")
    args = ap.parse_args()

    report: dict = {"host": args.host, "model": args.model, "when": time.strftime("%Y-%m-%d %H:%M:%S"), "steps": []}
    con = Console(args.host, args.user, args.password)

    if not args.skip_deploy:
        log(f"deploy: {args.title} -> /data/homebrew/{args.title_id}")
        n = con.deploy_title(args.title, args.title_id)
        log(f"  {n} files uploaded")
        report["steps"].append({"deploy": n})

    if not args.skip_launch:
        log("launch:")
        con.write("/data/PS5LM/launch.txt", f"{args.title_id}\n/data/homebrew/{args.title_id}\n")
        con.write("/data/PS5LM/model.txt", f"{args.model}\n")
        con.send_elf(args.launcher)
        report["steps"].append({"launch": "sent", "model": args.model})

    log(f"wait: up to {args.wait} s for /health on {args.host}:{HTTP_PORT}")
    t0 = time.time()
    if not wait_for_health(args.host, args.wait):
        log("  the server never answered; check /data/PS5LM/app.log and llama.log")
        print(con.read("/data/PS5LM/llama.log", 4000)[-2000:])
        return 2
    log(f"  ready after {time.time() - t0:.0f} s")

    _, props = http(args.host, "/props", timeout=20)
    report["props"] = {k: props.get(k) for k in ("model_path", "n_ctx", "default_generation_settings", "build_info")}
    log(f"props: model={props.get('model_path')} n_ctx={props.get('n_ctx')}")

    # 1. the chat path still works
    dt, chat = http(args.host, "/v1/chat/completions", {"messages": [{"role": "user", "content": "Say hello in five words."}], "max_tokens": 16})
    content = ((chat.get("choices") or [{}])[0].get("message") or {}).get("content")
    log(f"chat: {dt:.2f} s, {content!r}")
    report["chat"] = {"seconds": round(dt, 3), "content": content}

    # 2. logits at the last prompt position
    body = {
        "prompt": args.prompt,
        "n_predict": 0,
        "introspect": {"logits": {"top_k": 10, "entropy": True, "probs": True}, "positions": "last", "generated": False},
    }
    dt, doc = http(args.host, "/v1/introspect", body)
    got = top_of(doc)
    if got is None:
        log(f"introspect: NO RESULT ({json.dumps(doc)[:300]})")
        report["logits"] = {"error": doc.get("error")}
    else:
        e, lg = got
        log(f"logits (last): {dt:.2f} s  entropy {lg.get('entropy'):.3f} nats  argmax {lg['argmax']['piece']!r} "
            f"({lg['argmax']['logit']:.2f})  top: " + ", ".join(f"{t['piece']!r}:{t['logit']:.2f}" for t in (lg.get("top") or [])[:6]))
        report["logits"] = {"seconds": round(dt, 3), "index": e["index"], "entropy": lg.get("entropy"),
                            "argmax": lg["argmax"], "top": (lg.get("top") or [])[:10], "n_vocab": lg.get("n_vocab")}

    # 3. the whole vocabulary row
    body["introspect"]["logits"] = {"top_k": 3, "full": True, "max_full_rows": 1, "probs": False}
    dt, doc = http(args.host, "/v1/introspect", body)
    got = top_of(doc)
    full_len = None
    if got is not None:
        e, lg = got
        if "full" in lg:
            row = base64.b64decode(lg["full"]["data"])
            full_len = len(row) // 4
            log(f"full row: {dt:.2f} s  {len(row)} bytes, {full_len} float32  argmax id {lg['argmax']['id']}")
            report["full_row"] = {"seconds": round(dt, 3), "bytes": len(row), "n": full_len, "n_vocab": lg["n_vocab"]}
        else:
            log(f"full row: missing ({json.dumps(lg)[:200]})")

    # 4. activations at a few layers, including the last
    body["introspect"] = {
        "logits": {"top_k": 3},
        "positions": "last",
        "activations": {"tensors": ["l_out", "ffn_out", "attn_out", "result_norm"], "layers": [0, 1, 2, 12]},
        "generated": False,
    }
    dt, doc = http(args.host, "/v1/introspect", body)
    got = top_of(doc)
    if got is not None:
        e, _lg = got
        acts = e.get("activations") or {}
        shapes = {k: v["shape"][0] for k, v in sorted(acts.items())}
        log(f"activations: {dt:.2f} s  {len(acts)} tensors: {shapes}")
        report["activations"] = {"seconds": round(dt, 3), "tensors": shapes, "truncated": (doc["introspection"] or {}).get("truncated")}

    # 5. every prompt position
    short = "Once upon a time"
    body["introspect"] = {
        "logits": {"top_k": 3},
        "positions": "all",
        "activations": {"tensors": ["l_out"], "layers": [0, 12]},
        "generated": False,
    }
    body["prompt"] = short
    dt, doc = http(args.host, "/v1/introspect", body)
    it = doc.get("introspection") or {}
    n_entries = len(it.get("positions") or [])
    log(f"all positions: {dt:.2f} s for {it.get('meta', {}).get('n_prompt')} prompt tokens, {n_entries} entries")
    report["all_positions"] = {"seconds": round(dt, 3), "entries": n_entries, "n_prompt": it.get("meta", {}).get("n_prompt")}

    # 6. the model's own chat template
    body = {"messages": [{"role": "user", "content": short}], "n_predict": 0,
            "introspect": {"logits": {"top_k": 5}, "positions": "last", "generated": False}}
    dt, doc = http(args.host, "/v1/introspect", body)
    got = top_of(doc)
    if got is not None:
        e, lg = got
        log(f"messages: {dt:.2f} s  rendered {doc['introspection']['meta']['n_prompt']} tokens, "
            f"argmax {lg['argmax']['piece']!r}")
        report["messages"] = {"seconds": round(dt, 3), "n_prompt": doc["introspection"]["meta"]["n_prompt"],
                              "argmax": lg["argmax"]["piece"]}

    # 7. an intervention: zero one channel range in the middle of the MLP of layer 12
    base_top = (report.get("logits") or {}).get("top") or []
    body = {
        "prompt": args.prompt,
        "n_predict": 0,
        "introspect": {
            "logits": {"top_k": 10, "probs": True},
            "positions": "last",
            "generated": False,
            "interventions": [{"tensor": "ffn_out", "layers": [12], "channel_start": 0, "channel_end": 4096, "op": "zero"}],
        },
    }
    dt, doc = http(args.host, "/v1/introspect", body)
    got = top_of(doc)
    iv = (doc.get("introspection") or {}).get("interventions") or []
    if got is not None:
        e, lg = got
        log(f"intervention: {dt:.2f} s  modified {iv[0]['modified'] if iv else '?'} elements  "
            f"argmax now {lg['argmax']['piece']!r} ({lg['argmax']['logit']:.2f})")
        if base_top:
            log(f"  KL(top10 before || after) = {kl(base_top, lg.get('top') or []):.3f} nats")
        report["intervention"] = {"seconds": round(dt, 3), "modified": iv[0]["modified"] if iv else None,
                                  "argmax": lg["argmax"],
                                  "kl_top10": round(kl(base_top, lg.get("top") or []), 4) if base_top else None}

    report["llama_log_tail"] = con.read("/data/PS5LM/llama.log", 20000)[-4000:]
    report["app_log_tail"] = con.read("/data/PS5LM/app.log", 20000)[-2000:]

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(report, f, indent=1)
        log(f"wrote {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
