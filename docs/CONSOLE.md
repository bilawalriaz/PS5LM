# Console facts

Measured on the project's test console. Payload numbers come from `probes/memprobe` and `probes/threadprobe`, run through shsrv.

## Setup

- PS5 Slim (CFI-2015), firmware 13.60
- Relapse through WebKit Autoloader v0.5.2 and Payload Manager (PLK)
- Autoload: kstuff-lite 1.11, ShadowMountPlus 1.7beta2, etaHEN 2.5B, ftpsrv 0.21.1, shsrv 0.20, klogsrv 0.9
- FTP upload from a Mac on the same network: about 61 MB/s

The Relapse kernel stage sometimes hangs and the console then shuts down, or sticks on "preparing to turn off" until the power button is held. That is the exploit's race, not anything PS5LM runs.

## Memory, for one payload process

| What | Value |
|---|---|
| Flexible memory configured / free | 8.00 / 7.97 GiB |
| malloc ceiling (256 MiB steps, pages touched) | 6.25 GiB |
| Largest single block (`posix_memalign`) | 6.00 GiB |
| Direct memory | none: size 0, allocation fails with `0x80020023` |

The flexible pool is shared with the system: holding about 6 GiB froze the home screen until the process ended. Plan on 4 to 5 GiB for model, context and buffers in a payload. Bigger models (Qwen 3.8 27B) need a process with direct memory, that is a native app.

Why a payload has no direct memory: elfldr starts every payload as a new process with budget 0 (`sys_budget_set(0)` before it executes SceSpZeroConf, `elfldr.c` in ps5-payload-dev/elfldr), the budget of a system app. `0x80020023` is `0x80020000 + EAGAIN`: the pool is empty, the call is not refused. etaHEN's jailbreak request changes a process's rights (uid, sandbox, authid), not its budget, and kstuff patches no budgets; every homebrew project using more than 6 GiB is a native app.

## CPU, for one payload process

- AVX, AVX2, FMA, F16C, BMI2; no AVX-512. 16 logical CPUs online.
- **Affinity mask `0xea00`: only CPUs 9, 11, 13, 14 and 15**, shared with system services. Sony priority 700 (normal) for the main thread and new threads.
- 1, 2 and 4 threads busy for a second each: fine.
- Busy-waiting on all five (llama.cpp's defaults: one thread per physical core, `--poll 50`) starved the system: services stopped answering and the console shut down twice. `patches/0001-ps5-safe-cpu-defaults.patch` makes the default thread count the allowed CPUs minus two and turns polling off.
- `--poll 0` only ends the spinning between graphs. ggml's barrier spins at every operation inside a graph whatever the poll value, so during generation every worker runs flat out: the thread count is the real load setting.
- Only CPUs 14 and 15 are the two halves of one core (SMT siblings are 2k and 2k+1, as PS5SX2 and PS5CEMU-HAR lay them out). 9, 11 and 13 share their cores with threads outside the payload.
- llama.cpp's `-C`, `--cpu-strict` and `--prio` did nothing here: ggml took the PS5 for an unsupported platform. `patches/0002-ps5-thread-affinity-and-priority.patch` maps them to `scePthreadSetaffinity` and `scePthreadSetprio`, only ever lowers priority, and leaves the main thread unpinned. Not measured yet (#7).

## llama.cpp

First run, Qwen3.5 0.8B Q4_K_M, `-t 2 --poll 0 -lm none`, 512 context:

| | |
|---|---|
| Load | 4.4 s |
| Prompt | 38.5 tok/s |
| Generation | 14.2 tok/s |

`llama-server` loads Granite 4.2 3B (2.1 GiB) with mmap, so mapping a model file works in a payload.

Chat through `llama-server` and `ps5/webui` in the PS5's browser (browser open, `-c 4096 -np 1 -lm none`, 3 threads from the safe defaults):

| Model | Prompt | Generation |
|---|---|---|
| Qwen3.5 0.8B Q4_K_M, thinking off | 67 tok/s | 13.1 tok/s |
| Granite 4.2 3B Q4_K_M | 0.6 tok/s | 0.2 tok/s |

llama.cpp's own web UI renders blank in the PS5 browser; `ps5/webui` (plain ES5) works.

## Payload memory gets paged out

Models over ~1 GB ran 10 to 65 times slower than their size suggests (Granite 4.2 3B at 0.2 tok/s, Qwen3.5 2B at 0.9 tok/s). The reason is paging, not the architecture: while the PS5 browser is in front, the system pages a background payload's memory out to disk. An idle ps5lm was found holding 6 MB of a 600 MB model; the next request paged it back in at 0.3 tok/s.

Locking the weights (`-lm mlock`) fixes it: Qwen3.5 2B went from 0.9 to **8.5 to 9.2 tok/s** and stayed at 1.4 GB resident. Locked pages come out of the pool the home screen needs, though: 1.4 GB locked was fine, **2.7 GB (Qwen3.5 4B) froze the console**. ps5lm locks every model and refuses files over 1.6 GB.

## Downloads

- Sony's `libSceHttp2` cannot send a request from a payload (`sceHttp2SendRequest` fails); plain sockets, DNS and TCP 443 work. ps5lm uses cpp-httplib over mbedTLS 3.6.7 with the Mozilla CA list.
- The kernel caps `SO_RCVBUF` at 64 KB, whatever is asked for. With the Hugging Face CDN ~150 ms away, one connection tops out at 0.9 MB/s.
- 16 connections fetching 8 MiB pieces in parallel: **about 12.5 MB/s** (Qwen3.5 4B, 2.6 GB, in 217 s).

At exit, Sony's libc prints `[SceLibc] A heap error is detected` (SceLibcInternalHeap) after all work is done. After `main`, the SDK's start code unloads the modules the payload loaded and calls Sony's `exit()`, which runs the C++ static destructors while other threads may still run; the tools now leave through `_Exit` instead (`ps5/compat/exit.cpp`). Not yet checked on the console (#3).

## shsrv quirks

- Arguments are split on spaces with no quoting: put prompts in a file (`-f`).
- Programs need an absolute path; a relative one, and very long command lines, end with exit code 3 ("command not found").
- `kill` takes no `-9`; Payload Manager's `/process_kill?pid=N` works.
- `browse <URL>` opens the PS5's web browser.

## The native app on the GPU

PS5LM's app (`ps5/app`, PPSA99575) is a title, not a payload: it gets the console's direct memory and the GPU, through ggml-vulkan on PS5_Vulkan's RADV.

| What | Value |
|---|---|
| Direct memory for the title | 12 GiB; RADV reports one 11.44 GiB device heap with `radv_enable_unified_heap_on_apu` (8 GiB without) |
| Device | PlayStation 5 GPU (RADV NAVI21), Vulkan 1.4 |
| Qwen3.8-27B UD-IQ2_XXS, `-ngl 999 -lm none -c 4096 -ub 64` | loads in 82 s |
| Prompt | 35 to 50 tok/s |
| Generation | 21 tok/s on a short prompt, 9 tok/s after the web UI's 371-token prompt |

What a title has to do differently from a payload, each found by a crash:

- **Never exit.** `exit()` and `_Exit` end in SIGSYS; the title waits for the shell to close it, as ps5-native-app-boilerplate's examples do. It hides the splash screen itself and posts notifications to show its state.
- **Imports that resolve to nothing.** Functions only `libScePosixForWebKit` or `libkernel_sys` define in the SDK's stubs are null in a title: `isatty` (llama.cpp's log setup jumped to address 0), `getnameinfo`, `mkstemp`, `readlink`, `link`, `symlink`, `pathconf`, `fork`. `ps5/app/compat_app.c` defines them, in an archive so the title exports nothing.
- **The SDK fork's `getaddrinfo` always fails**, and cpp-httplib resolves even the address it binds; `compat_app.c` has a numeric one. Its `accept4` is the title's own too, and logs `errno` when `accept` fails: with the payload's version the listener stopped 20 s into the first run.
- **No `HOME`**: llama.cpp throws looking for its cache directory without one.
- C++ exceptions and RTTI are off by default for the PS5 target and must be turned on for llama.cpp.

### Switching models

Unloading a model in the running app leaves part of its GPU memory held: with Qwen3.8-27B UD-IQ2_XXS at 64k, the pool went from 11.01 GiB free before loading to 5.77 GiB free after `llama_server` returned, and ggml's Vulkan figure fell the same way, so the next model could not get its KV cache. The app therefore changes model by restarting itself: it writes the choice to `/data/PS5LM/model.txt` (read once by the new process, then deleted, so a plain launch loads nothing and opens the library) and calls `sceSystemServiceLoadExec("/app0/eboot.bin", NULL)`. The system replaces the process (`Kill for LoadExec(0x11d) => 0`, then a new `EXEC`) without the crash reporter, and the new process starts from the whole pool (11.01 GiB free). Where the memory is held is still open.

### The dashboard on the console

ps5-homebrew-ui draws through ps5-opengl in the same title as RADV: the GL stack is linked as one object with only its GL/EGL API global (4,399 symbol names collide with RADV's Mesa otherwise), `sceAgcInit` runs once for both drivers (`--wrap`), and the GL runtime's thread-local initialiser `_ZTH23_mesa_glapi_tls_Context` is an empty function, as ProsperoAI has it. The screen costs about 0.35 GiB of the pool (11.38 to 11.03 GiB free). The app can be driven from a PC without a controller: `scripts/ps5lm-app.sh close|deploy|launch|screenshot|load|unload`.

ShadowMountPlus registers a title once and copies its icon and backgrounds then; replacing them later needs a re-registration. Uninstalling to force one hit "Register failed: TitleDir bridge unavailable" after ShadowMountPlus had logged `stale cave bridge detached`; reloading ShadowMountPlus fixed it.

## Starting it from the PC

`ps5-homebrew-dev-protocol`, whose launch payload `scripts/ps5lm-app.sh launch`
used to build, is not public any more (its repository answers 404). The
replacement ships here: `ps5/launch/launch.c`, built by `scripts/build-launch.sh`
into `build/launch/launch.elf` and sent to elfldr on 9021. It reads
`/data/PS5LM/launch.txt` first — the title id on the first line, and optionally a
`/data/homebrew/...` directory to hand to ShadowMountPlus's manual list — then
launches the title through `sceSystemServiceLaunchApp`, refusing when another
title is already running or nobody is signed in. `scripts/ps5lm-app.sh launch`
writes that file and sends the payload, so the flow from the PC is still:

```sh
PS5_HOST=<console> scripts/ps5lm-app.sh deploy
PS5_HOST=<console> scripts/ps5lm-app.sh launch
```

A title folder copied to `/data/homebrew/<id>` shows up in ShadowMountPlus by
itself; the manual list is only for directories outside it.

## Verifying introspection on a console

`scripts/introspect-console-test.py` does the whole sequence from the PC: upload
the title, write `launch.txt` and `model.txt`, send the launcher, wait for
`/health`, then exercise the endpoint and print what came back.

```sh
python3 scripts/introspect-console-test.py --host <console> \
    --title build/app-ps5/title/PPSA99575 \
    --model /data/PS5LM/models/MiniCPM5-2B-Q8_0.gguf \
    --json-out console-introspect.json
```

In order it checks: a plain chat completion (the normal path still works), logits
at the last prompt position with entropy, one full-vocabulary row, a handful of
activations, every prompt position, the chat-template path, and one intervention
with the KL divergence of the top-10 distribution before and after. The JSON it
writes is the record worth keeping: timings, argmax, tensor shapes and the app
and server log tails.

### What the check returned (2026-10-18, firmware 13.60)

Run against the release `v0.10.0` artifact (`eboot.bin` sha256
`ebaea7d0c28cba06f8a9135d023dd68d7b22bb0a48623181e60954014df5aef0`), on MiniCPM5-2B Q8_0
(2.68 GB) loaded on the GPU with `-c 65536`, server build `b11608-04f7226a2`. The whole
suite above passed:

| Step | Result |
|---|---|
| Launch to `/health` (model load included) | 77 s |
| Logits at the last prompt position | 0.07 s, entropy 1.628 nats, argmax `' there'` (14.26) |
| Full vocabulary row | 1.13 s, 522,240 bytes = 130,560 float32 = exactly `n_vocab` |
| 13 activation tensors (l_out/ffn_out/attn_out for layers 0, 1, 2, 12, plus `result_norm`) | 0.20 s, 2048 floats each |
| Every prompt position (4 tokens) | 0.27 s |
| Chat-template path | 0.08 s, 15 rendered tokens, argmax `'We'` |
| Intervention: zero `ffn_out-12` channels 0..4095 | 0.10 s, `modified: 20480` (4096 x 5 rows), argmax `' there'` -> `' a'`, top-10 KL 0.915 nats |
| `positions: "all"` over a 103-token prompt, 2 tensors x 3 layers | 1.51 s, 103 entries, 6.86 MB JSON, `truncated: false` |
| `/completion`, `/v1/chat/completions` | unaffected (the chat path answers in `reasoning_content`, a reasoning model's channel) |

The cross-check that matters for the Vulkan path: the same token's nine captured tensors,
and its logits, are **byte-identical** whether they came from the batched prefill
(`positions: "last"`) or from one-token-at-a-time decoding (`positions: "all"`) — so the
`cb_eval` capture returns the same rows on the GPU backend as on the CPU one.

Measured limits: 103 tokens of stepping cost 1.51 s, i.e. the console sustains roughly
70 tokens/s while capturing activations for six tensors per position.
