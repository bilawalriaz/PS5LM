# Inspecting the model (llama-inspect)

`llama-server` shows what a chat client needs: the text, the top-N logprobs of the
tokens it sampled, timings, metrics. It deliberately does not expose the rest of the
model — llama.cpp's server README lists "getting the intermediate activation from API"
as out of scope, because its scheduler interleaves slots and a tensor's second
dimension is then some mixture of sequences.

Experiments need the rest. `llama-inspect` is a second tool in this repository, built
from the same llama.cpp, that runs one model on one context and one request at a time.
That makes every capture unambiguous:

- **logits** — the full next-token vector, not just the sampled token's probability, at
  any position of the prompt;
- **activations** — any tensor of the compute graph, by name, with per-token summary
  statistics or the raw `float32` values;
- **interventions** — edit those tensors in place (zero, scale, add a channel) and see
  what the model answers next;
- **generation** — sample tokens and collect each step's logits and activations.

This is the tool for "which neurons make it say that", not for serving users.

Its counterpart inside the app is `POST /v1/introspect` (patch 0009): the same
logits, activations and channel interventions, but in the running server, on the
GPU, with the model the app has loaded and the prompt the app would really send —
so a model too big for a payload is still available to an experiment. See
[docs/INTROSPECTION.md](INTROSPECTION.md). Use it when the model must be large or
kept loaded; use `llama-inspect` when one context per request, `baseline: true` and
per-token summaries matter more.

## Build

On a workstation:

```sh
cmake -S third_party/llama.cpp -B build/host -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_BUILD_SERVER=ON -DGGML_NATIVE=ON -DLLAMA_CURL=OFF
cmake --build build/host --target llama-inspect -j
```

For the console, the payload build does it as part of the usual flow: it needs the
ps5-payload-dev SDK (`scripts/setup-sdk.sh`) and a host LLVM (`llvm-config-18..21` in
`PATH`, `clang-18`+ installed):

```sh
source scripts/env.sh
scripts/build-llama.sh llama-inspect         # build/llama-ps5/bin/llama-inspect.elf
```

The tool is in `scripts/build-llama.sh`'s default target list, so a plain
`scripts/build-llama.sh` builds it alongside `llama-cli` and `llama-server`.

## Run

```sh
# workstation
build/host/bin/llama-inspect -m models/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf -c 2048

# console (FTP the model to /data/PS5LM/models first)
scripts/run.sh llama-inspect -m /data/PS5LM/models/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf \
    -c 2048 --host 0.0.0.0 --port 8090
```

It listens on **port 8090**, on **127.0.0.1** unless `--host` says otherwise. There is
no authentication: leave it on loopback on a machine with other users, and pass
`--host 0.0.0.0` only on a network you trust (the `llama-server` example in
`scripts/run.sh` does the same).

All the usual llama.cpp flags apply (`-c`, `-ngl`, `-t`, `-fa`, `--no-mmap`, ...).
Context size is capped by the model; the request batch is capped at 128 tokens a
decode, larger prompts are chunked automatically.

## Talk to it

```sh
python3 scripts/ps5lm_inspect.py --host 10.101.1.112 info
python3 scripts/ps5lm_inspect.py --host 10.101.1.112 forward --prompt "The capital of France is" --top-k 5
python3 scripts/ps5lm_inspect.py --host 10.101.1.112 tensors --prompt "Hello"
python3 scripts/ps5lm_inspect.py --host 10.101.1.112 capture --prompt "The capital of France is" \
        --tensors '^ffn_gate-20$' --top-k 8
```

`scripts/ps5lm_inspect.py` is a stdlib-only client; `InspectClient` is importable from
a notebook. Everything below is the HTTP surface it uses.

### `GET /health`, `GET /v1/inspect/info`

`info` returns the model's shape: `n_layer`, `n_embd`, `n_head`, `n_head_kv`,
`n_vocab`, `n_ctx`, `n_ctx_train`, `n_params`, `desc`, the llama.cpp `build` string.

### `POST /v1/inspect/tokenize`

```json
{"text": "The capital of France is", "add_special": true, "parse_special": true}
```

Returns `tokens` as `[{"id": 785, "piece": "The"}, ...]`. Use the ids with the
`tokens` field of the other endpoints when you want a prompt the tokenizer cannot
reproduce (leading spaces matter).

### `POST /v1/inspect/tensors`

Runs one small forward and lists every graph node that was executed — name, type,
shape `ne` (ggml order: `ne[0]` is the feature axis) and `token_axis`. Use it to find
the names to capture. Roughly 700 nodes for a small Llama; the interesting families
for a layer `L`:

- `ffn_gate-L`, `ffn_up-L`, `ffn_down-L` — the SwiGLU FFN. `ffn_gate-L` is
  `silu(gate_proj(x))` *after* the activation, `ffn_up-L` is `up_proj(x)`, and
  neuron `j` contributes through channel `j` of both. Zeroing channel `j` of
  `ffn_gate-L` removes neuron `j` from the layer's output.
- `ffn_silu-L`, `ffn_swiglu-L` (arch dependent) — the same values under other names.
- `kqv_out-L` — the attention block's output, after the output projection
  (`[n_embd/n_head, n_head, n_tokens]`, so later heads start at head `h` * head_dim).
- `norm-L` — the attention norm's output at the start of layer `L`.
- `inp_embd`, `inp_pos` — the token embedding and positions, once.

### `POST /v1/inspect/forward` — the main endpoint

```json
{
  "prompt": "The capital of France is",
  "logits":   {"positions": "last", "top_k": 20, "raw": false},
  "capture":  {"tensors": ["^ffn_gate-20$"], "mode": "summary", "top_k": 8},
  "interventions": [{"tensor": "^ffn_gate-20$", "channels": [601], "op": "zero"}],
  "baseline": true
}
```

`prompt` (text) or `tokens` (ids). `add_special` defaults to true.

**`logits`**

- `positions`: `"last"` (default), `"all"` (every prompt position, limited to 2048
  tokens), `"indices"` with an explicit `indices` array (negative counts from the end,
  so `-1` is the last token) or `"none"`.
- `top_k`: how many tokens to rank by logit, with their probability.
- `raw`: include the whole vocab's logits as base64 `float32`, little-endian
  (`{\"encoding\": \"f32le-base64\"}`). About 600 KB per position for a 151k vocab.

Each entry also carries `entropy` (nats, over the full distribution).

**`capture`**

- `tensors`: regexes matched against node names (`^ffn_gate-\d+$`); unmatched layers
  simply produce nothing.
- `mode`: `"summary"` (default), `"raw"`, or `"names"`.
  - `summary` gives, per token, `mean`, `std`, `min`, `max`, `l2` and the `top` `k`
    values by absolute value with their feature `index` — enough to find which
    neurons are active without the payload of the whole vector.
  - `raw` gives the tensor as dense `float32` base64, row-major in `ne` order, capped
    by `max_raw_bytes` (default 64 MiB).
  - `names` lists names without values (what `POST /v1/inspect/tensors` uses).
- `positions`: keep only these token indices (global, from the start of the prompt).
- `top_k`: how many entries per token in `summary` mode (0 = statistics only).

**`interventions`** — applied during the same forward, before the capture is read, so
the returned activations are the modified ones:

- `tensor`: regex, as above.
- `channels`: explicit feature indices; or `channel_start`/`channel_end` (half open);
  or neither, meaning every channel.
- `position`: which token to edit (default: every token).
- `op`: `"zero"`, `"scale"` (`value`) or `"add"` (`value`).

The response's `interventions` array says what matched and how many values were
written (`matched`, `modified`), so a typo in a tensor name is visible instead of
silently doing nothing.

**`baseline`** — when true, the endpoint runs the prompt twice: once untouched, once
with the interventions, and puts the untouched result under `baseline`. Use it for
causal claims: the difference between `baseline` and the top-level result is the
intervention's effect on the same tokens, the same positions, the same weights.

### `POST /v1/inspect/generate`

```json
{"prompt": "...", "max_tokens": 32, "temperature": 0.0, "top_k": 40,
 "logits_top_k": 10, "capture": {"tensors": ["^ffn_gate-20$"], "mode": "summary"},
 "capture_each": true, "seed": 0}
```

Greedy by default (`temperature` 0), so a comparison between two runs differs only by
the intervention. Every step returns the sampled token, its top logits and — when
`capture_each` is set — that step's activations, with `step: -1` holding the prompt's.
Stop tokens end the loop.

## Recipes

**Which neurons separate two prompts** (the full loop, `examples/interpret/neuron_probe.py`):

```sh
python3 examples/interpret/neuron_probe.py --host 10.101.1.112 --layer 20 \
    --prompt-a "The capital of France is" --prompt-b "The capital of Germany is" --target " Paris"
```

It captures `ffn_gate-20` raw for both prompts, ranks neurons by the difference at the
last token, then zeroes the top ones with `baseline: true` and reports how the target
token's probability and the whole next-token distribution (KL) moved. On a 0.5B model
this typically moves the target's probability by several points.

**Steer instead of ablate** — `{"tensor": "ffn_gate-20", "channels": [601], "op": "scale",
"value": 3.0}` amplifies a neuron rather than removing it, and `"op": "add"` shifts it.

**Follow one token through the network** — `capture` with `["^(kqv_out|ffn_gate)-\d+$"]`
and `mode: "summary"`, `top_k: 4` gives, for every layer, which channels were loudest
at the last token: the classic "what changes when the answer flips" table.

**Heads** — capture `kqv_out-L` raw and slice `[head*head_dim, (head+1)*head_dim)` to
zero one attention head's output contribution.

**Reproduce a sampled token** — `logits.positions: "indices", indices: [-1], raw: true`
returns the full distribution the sampler saw for the last position.

## Limits, honestly

- **CPU only in this build.** The native PS5LM app gets the GPU through RADV; the
  payload tools don't. On the console's Zen 2 that is fine for forward passes and
  ablations, not for chat; measure the model with `llama-bench` before assuming a
  speed.
- **One request at a time.** Requests are serialised on the context's mutex.
- The request batch is 128 tokens; longer prompts are decoded in chunks, and
  `logits.positions: "all"` requires the positions to fit the same limit per chunk.
- `capture` sees the graph the model actually runs. If a layer uses flash attention,
  the attention tensors have flash attention's shapes; `ffn_*` is unaffected.
- Quantised tensors cannot be read as floats, and the endpoint reports them as
  `truncated` instead of guessing.
- The `raw` tensors are `float32` conversions of whatever the graph computed (`f32`,
  `f16` or `bf16` activations), converted on the host after the node ran.

## Safety

The tool is unauthenticated and can read the model's internals and write to its
activations. Treat it like a debugger: bind it to loopback by default, and if you bind
it to `0.0.0.0` on the console, remember the console shares that network with whatever
else is on it.
