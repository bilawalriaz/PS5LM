# Introspection: raw logits and layer activations

PS5LM's server normally answers like any chat server: text in, text out. For
interpretability work that is not enough — you want the distribution the model
actually computed, and the numbers inside the transformer while it ran.

This fork adds one endpoint that returns both, for the positions and layers you
name:

```
POST /v1/introspect
```

It is the same request as `/completion` (same tokenizer, same sampler, same
context, same model) plus an `introspect` field that says what to capture. The
answer is the normal completion object with an extra `introspection` member.
The field also works on `/completion` itself, so an existing client only has to
add one key.

Implementation: `patches/0009-introspection-logits-and-activations.patch` over
llama.cpp `04f7226`. Logits come from the context's own output buffer
(`llama_get_logits_ith`), so no work is repeated; activations are copied out of
the compute graph while it runs, through the graph-eval callback llama.cpp
already exposes (`llama_context_params.cb_eval`). Nothing is captured unless a
request asks, and nothing is recomputed.

## Quick start

```sh
# the console must be running a model; then, from a computer on the same network
python3 scripts/ps5lm-introspect.py --host <console-ip> \
    --prompt "Once upon a time" --positions last --top-k 10 \
    --tensors l_out --layers 12
```

```sh
curl -s http://<console-ip>:8081/v1/introspect -H 'Content-Type: application/json' -d '{
  "prompt": "Once upon a time",
  "n_predict": 0,
  "introspect": {
    "logits": {"top_k": 10, "entropy": true},
    "positions": "last",
    "activations": {"tensors": ["l_out"], "layers": [12]}
  }
}'
```

## The request

| Field | Meaning |
|---|---|
| `prompt` | Text, exactly as `/completion` takes it |
| `messages` | Chat turns (`[{role, content}]`); the model's own chat template is applied first, so the probed prompt is the prompt the model would really be asked |
| `n_predict` | Tokens to generate. `0` captures the prompt only (default) |
| `temperature`, `top_k`, … | The usual sampling fields; `0` is greedy |
| `introspect.logits` | `true`, or an object: `top_k` (candidates per position), `entropy`, `probs`, `full` (the whole vocabulary row), `max_full_rows` (cap, default 8) |
| `introspect.positions` | `"last"` (the final prompt token, default), `"all"` (every prompt position), or a list of indices |
| `introspect.activations.tensors` | Tensor names to capture; `l_out` alone is a typical choice |
| `introspect.activations.layers` | `"all"` or a list of layer indices |
| `introspect.generated` | Also capture the generated tokens (default `true`) |

## The answer

```json
{
  "content": ", there was a",
  "introspection": {
    "meta":   {"model": "…", "n_layer": 42, "n_vocab": 130560, "n_ctx": 65536, "n_prompt": 5, "request": {…}},
    "inputs": [{"index": 0, "id": 1, "piece": "<s>"}, …],
    "positions": [
      {
        "index": 4, "id": 261, "piece": " time", "generated": false,
        "logits": {
          "n_vocab": 130560,
          "argmax": {"id": 11, "piece": ",", "logit": 17.79, "prob": 0.99},
          "top":    [{"id": 11, "piece": ",", "logit": 17.79, "prob": 0.99}, …],
          "entropy": 0.157,
          "full":   {"encoding": "base64", "dtype": "f32", "n_vocab": 130560, "data": "…"}
        },
        "activations": {
          "l_out-12": {"shape": [2048, 1, 1, 1], "dtype": "f32", "encoding": "base64", "data": "…"}
        }
      }
    ],
    "truncated": false
  }
}
```

- `index` is the position in `inputs`; generated tokens continue after the
  prompt, with `"generated": true` and `"step"` counting from 0. The final
  generated token has no entry: no logits are computed for it.
- `full.data` is `n_vocab` little-endian float32s, base64-encoded; `activations
  .data` is one row of the tensor, i.e. that token's activations.
- `truncated` says the activation budget cut tensors out of some positions.

## Which activations

Tensors are named by llama.cpp's graph; `-<layer>` is appended for per-layer
ones. For the `llama` architecture (which is what MiniCPM5-2B, Qwen, Mistral,
LFM2 and most GGUFs declare):

| Tensor | What it is |
|---|---|
| `inp_embd` | Token embedding + position: the residual stream entering block 0 |
| `attn_norm-<il>` | Residual stream after the block's input norm |
| `attn_out-<il>` | Attention output, before the residual add |
| `ffn_norm-<il>` | Residual stream after the post-attention norm |
| `ffn_out-<il>` | MLP output, before the residual add |
| `l_out-<il>` | Residual stream after block `il`; `l_out-<il-1>` is the input of block `il` |
| `result_norm` | Final hidden state, before the output projection |
| `result_output` | The logits tensor itself |

Other architectures name their tensors in `src/models/<arch>.cpp`; a name that
does not exist is simply not captured, and the answer then has no activations
for it.

## Costs and limits

- **`positions: "all"` evaluates the prompt one token at a time** so that every
  position is the newest row of its own graph. A 1000-token prompt therefore
  costs about 1000 decode steps (on the console, that is tens of seconds, not
  milliseconds). `"last"` keeps the normal batched prefill.
- **`entropy`/`probs` walk the whole vocabulary** for every captured position
  (an exp over `n_vocab` values). Leave them off when capturing thousands of
  positions.
- **`full` rows are ~4 bytes × `n_vocab` each** (≈ 500 KB per position for a
  130k vocabulary) and are capped by `max_full_rows`.
- **Activations are 4 bytes × `n_embd` per tensor per position** (8 KB for a
  2048-wide model). The response is base64, so a request for every layer and
  every tensor over a long prompt can be very large; the server stops adding
  them after `LLAMA_INTROSPECT_MAX_BYTES` (default 256 MiB) and sets
  `"truncated": true`.
- The request occupies the server's slot like any generation, and
  `positions: "all"` disables prompt caching for that request.

## Interventions: change an activation, not just read it

The same request can modify a tensor in place while the model runs — zero, scale
or add a value to one channel (a single FFN neuron, one attention-head
dimension) or a range of channels, at every position or at one position. That
turns "this neuron is active when it says X" into "and with it zeroed, does it
stop saying X?".

```json
{
  "prompt": "Once upon a time",
  "n_predict": 0,
  "introspect": {
    "logits": {"top_k": 10, "probs": true},
    "positions": "last",
    "interventions": [
      {"tensor": "ffn_out", "layers": [12], "channels": [3456], "op": "zero"}
    ]
  }
}
```

| Field | Meaning |
|---|---|
| `tensor` | Base name (`ffn_out`) or an exact node name (`ffn_out-12`) |
| `layers` | Which layers a base name expands to (default: all) |
| `channels` | Channel indices to change; or `channel_start`/`channel_end` (end exclusive); neither means the whole row |
| `position` | Token index of the request; default `-1` = every position |
| `op` | `zero`, `scale` (multiply by `value`) or `add` (`value`) |
| `value` | Operand of `scale`/`add` |

The answer repeats each intervention with `"modified"`, the number of elements
actually changed — 0 means the name never matched a tensor in this graph, so
check it rather than assuming the intervention happened.

Notes that matter:

- The change is applied inside the graph-eval callback, i.e. after llama.cpp has
  finished computing that tensor and before anything consumes it, so it affects
  exactly the forward pass that follows.
- A `position` intervention forces the prompt to be evaluated one token at a
  time, and *any* intervention that changed something makes the server drop the
  prompt cache afterwards: the KV entries of an intervened pass are not the ones
  a later request should reuse.
- Interventions need the tensor to be `float32`/`float16`/`bfloat16` and at most
  64 MiB; anything else is left alone.
- Compare against a baseline by sending the same request twice, once without
  `interventions`.

## Switches

Set in `/data/PS5LM/env.txt` (one `NAME=value` per line; the app applies
`LLAMA_*` names before the server starts):

| Name | Default | Meaning |
|---|---|---|
| `LLAMA_INTROSPECT` | `1` | `0` disables the endpoint (it then answers 501) |
| `LLAMA_INTROSPECT_MAX_BYTES` | `268435456` | Cap on the activations one request may return |

## What this does not give you

- **A separate context per request.** This endpoint runs in the app's server, so it shares the
  app's loaded model, its slot and its KV cache. The standalone
  [`llama-inspect`](INSPECT.md) tool is the opposite trade: its own process, its own context, one
  request at a time (so a capture is the whole batch, and `baseline: true` runs a prompt both ways in
  one call) — CPU only, and a payload's memory limit.
- **Weights.** They are in the GGUF file: read it directly from the host
  (`gguf-py`, `gguf_dump.py`) or over FTP; the console does not need to serve
  them.
- **Attention matrices / per-head scores.** llama.cpp does not keep them; the
  graph tensors above are what it computes.
- **Embeddings for retrieval.** That is upstream's `/v1/embeddings` (start the
  server with `--embeddings`), unchanged by this fork.
- **Gradients.** llama.cpp is forward-only.

## Status

- The patch builds and is exercised on the host (macOS, CPU backend) with a
  small test model: logits, full rows, entropy, all-positions capture, the
  chat-template path, the byte budget and the disabled mode were all run, and
  the same token's activations match between the batched and step-by-step
  paths.
- On the console (PS5, ggml-vulkan on RADV) see
  `docs/CONSOLE.md#introspection` for what has been measured there.
