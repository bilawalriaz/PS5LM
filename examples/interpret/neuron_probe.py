#!/usr/bin/env python3
"""Find the FFN neurons that separate two prompts, then ablate them and measure.

    python3 examples/interpret/neuron_probe.py --host 10.101.1.112 \\
        --layer 20 --prompt-a "The capital of France is" --prompt-b "The capital of Germany is" \\
        --target " Paris"

The probe:
  1. runs both prompts, capturing the raw `ffn_gate-<layer>` tensor (the SwiGLU
     gate after its activation, before the multiply with `ffn_up`);
  2. ranks neurons by the difference between the two prompts at the last token;
  3. zeroes the top-ranked neurons in one forward pass and measures what changes:
     the target token's probability and the KL divergence of the next-token
     distribution.

Zeroing `ffn_gate` channel j removes neuron j for that token; nothing else in
the block depends on the channel.

Requires scripts/inspect.py next to this file's repository root, and a
llama-inspect process on the console (see docs/INSPECT.md).
"""

from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))

from ps5lm_inspect import InspectClient, InspectError, decode_f32  # noqa: E402


def last_token_activations(result: dict, name: str):
    """Return (ne, values of the last token in the captured tensor)."""
    for a in result.get("activations", []):
        if a["name"] != name:
            continue
        if a.get("encoding") != "f32le-base64":
            raise SystemExit(f"{name}: expected raw capture, got {a.get('encoding')}")
        values = decode_f32(a)
        ne = a["ne"]
        n_tokens = ne[1]
        row = ne[0]
        if n_tokens != result["n_tokens"]:
            raise SystemExit(f"{name}: captured {n_tokens} tokens, prompt has {result['n_tokens']}")
        start = (n_tokens - 1) * row
        return ne, values[start:start + row]
    raise SystemExit(f"{name}: not captured; check the layer number and that the model has FFN tensors")


def distribution(logits_entry: dict) -> tuple[list[str], list[float], dict[int, float]]:
    """Full next-token distribution from a raw logits entry."""
    values = decode_f32(logits_entry)
    mx = max(values)
    ex = [math.exp(v - mx) for v in values]
    z = sum(ex)
    probs = [e / z for e in ex]
    return [], probs, {i: p for i, p in enumerate(probs) if p > 0}


def kl(a: dict[int, float], b: dict[int, float]) -> float:
    out = 0.0
    for i, p in a.items():
        q = b.get(i, 0.0)
        if q <= 0:
            continue
        out += p * math.log(p / q)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="neuron separation + ablation probe")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--layer", type=int, required=True)
    ap.add_argument("--tensor", default="ffn_gate", help="graph tensor stem (default: ffn_gate)")
    ap.add_argument("--prompt-a", required=True)
    ap.add_argument("--prompt-b", required=True)
    ap.add_argument("--target", default=None, help="token whose probability is tracked, e.g. ' Paris'")
    ap.add_argument("--top", type=int, default=8, help="neurons to rank and ablate together")
    ap.add_argument("--each", action="store_true", help="also ablate each neuron on its own")
    args = ap.parse_args()

    client = InspectClient(args.host, args.port)
    tensor = f"{args.tensor}-{args.layer}"

    try:
        a = client.forward(args.prompt_a, capture={"tensors": [tensor], "mode": "raw"}, raw_logits=True, top_k=5)
        b = client.forward(args.prompt_b, capture={"tensors": [tensor], "mode": "raw"}, raw_logits=True, top_k=5)
    except InspectError as e:
        print(f"inspect: {e}", file=sys.stderr)
        return 1

    ne, act_a = last_token_activations(a, tensor)
    _, act_b = last_token_activations(b, tensor)
    n_ff = ne[0]

    ranked = sorted(range(n_ff), key=lambda j: abs(act_b[j] - act_a[j]), reverse=True)[: args.top]

    base_logits = a["logits"][0]
    _, base_probs, base_dist = distribution(base_logits)

    target_id = None
    if args.target:
        tok = client.tokenize(args.target, add_special=False)["tokens"]
        if not tok:
            raise SystemExit("target tokenised to nothing")
        target_id = tok[0]["id"]

    def measure(label: str, channels: list[int]) -> None:
        iv = [{"tensor": tensor, "channels": channels, "op": "zero"}]
        try:
            r = client.forward(args.prompt_a, capture={"tensors": [tensor], "mode": "raw"},
                               baseline=True, interventions=iv, raw_logits=True, top_k=5)
        except InspectError as e:
            print(f"{label}: {e}", file=sys.stderr)
            return
        ablated = r["logits"][0]
        _, _, abl_dist = distribution(ablated)
        top = ablated["top"][0]
        line = f"{label:<28} top1={top['piece']!r:<18} KL={kl(base_dist, abl_dist):.4f}"
        if target_id is not None:
            line += f"  p(target) {base_dist.get(target_id, 0):.6f} -> {abl_dist.get(target_id, 0):.6f}"
        print(line)

    print(f"model      {a['model']}")
    print(f"tensor     {tensor}  ne={ne}  {n_ff} neurons per token")
    print(f"top1 A     {base_logits['top'][0]['piece']!r}")
    print(f"top1 B     {b['logits'][0]['top'][0]['piece']!r}")
    print()
    print(f"{'rank':>4} {'neuron':>7} {'act(A)':>10} {'act(B)':>10} {'delta':>10}")
    for rank, j in enumerate(ranked):
        d = act_b[j] - act_a[j]
        print(f"{rank:>4} {j:>7} {act_a[j]:>10.4f} {act_b[j]:>10.4f} {d:>10.4f}")
    print()

    measure(f"ablate top {len(ranked)} together", ranked)
    if args.each:
        for rank, j in enumerate(ranked):
            measure(f"ablate neuron {j} (rank {rank})", [j])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
