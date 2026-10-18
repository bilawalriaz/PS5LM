# Changelog

Versions follow [semver](https://semver.org). The app's version is set in
`ps5/app/version.hpp` and shown on the dashboard; each release is tagged
`v<version>` and ships `ps5lm-app-v<version>.zip` (see `scripts/release-app.sh`).
v0.1.x are the upstream payload releases by cobanov.

## [Unreleased]

### Added
- **The control port** (8083), an HTTP API for agents and scripts that use the console as an
  inference server and must get it back when it gets stuck. `GET /status` gives a verdict (`ready`,
  `busy`, `slow`, `wedged`, `loading`, `restarting`, `no_model`, `failed`) together with `/health`,
  the age of the last task llama-server's main loop finished, the model, memory and load.
  `POST /restart` restarts with the same model, another model or other arguments: clean, or `hard`
  for a wedged server. `POST /load` and `POST /unload` switch or drop the model.
  `GET /logs/{llama,app,ui,restarts}` returns a log's tail or reads from an offset. `env.txt`,
  `settings.json` (`introspect`, `ctx_cap`, ...) and a watchdog (off by default) can be read and
  changed. It runs in the app on its own threads, outside llama-server, so it answers when the
  server's main loop is stuck and `/health` is the only thing that still does. A restart the main
  loop does not carry out within 8 s (hard) or 30 s (clean) is done by the control thread itself.
  Every route but `/ping` needs the token in `/data/PS5LM/control-token.txt`.
  See [docs/CONTROL.md](docs/CONTROL.md).
- `scripts/ps5lm_ctl.py`, the client: `status`, `ensure` (ready, or restart and wait), `restart`,
  `load`, `unload`, `wait-ready`, `logs --follow`, `env`, `settings`, `watchdog`. It finds the token
  over FTP. Standard library only, and importable (`Control.ensure_ready()`).
- `scripts/control-host-test.sh` runs the control port on a PC against a fake llama-server that can
  be made to wedge (22 checks).
- Run on the console: status, logs, env, clean and hard restarts, and recovery from a real wedge,
  both by `ensure` (68 s) and by the watchdog on its own. Details in docs/CONTROL.md.

### Fixed
- Logging no longer stalls the server. llama.cpp's log queue holds 512 lines, and when it was full
  `common_log_add()` waited for the writer thread, which prints every line to `app.log` and to
  `llama.log` with a flush each. Under a rapid introspection sweep (~180 lines a second) the writer
  fell behind and the server thread froze mid-`print_timings`: `/slots` and `/metrics` hung while
  `/health` stayed 200. On the console a full queue now drops the line and counts it, and the writer
  logs `N log lines dropped, the log writer fell behind` once it catches up
  (`patches/0010-log-never-blocks.patch`). With it, `LLAMA_ARG_LOG_VERBOSITY=2` in `env.txt`
  should no longer be needed to keep a sweep alive. On the console (MiniCPM5-2B Q8_0, default
  verbosity), 3,000 back-to-back uncached `/v1/introspect` ablations all answered (p50 48 ms, p99
  79 ms, one 10.6 s stall ridden out) and `/slots` answered in 12 ms afterwards; before, about 300
  such requests froze the server for good.

## [0.10.1] - 2026-10-18

### Fixed
- `argmax.prob` is the argmax's real probability. It was hardcoded to `1.0`, so an answer that did not
  ask for the full row reported a certainty the model did not have. The console run exposed it (an
  entropy of 1.176 nats printed next to `p=1.0000`); the value is now `exp(-logsumexp)` and matches
  `softmax()` of the full vocabulary row to 1e-9. The top-k probabilities were always correct.
- `docs/INTROSPECTION.md` now lists the activation tensors this revision actually delivers on both
  backends, measured on the console rather than assumed: `embd`, `attn_norm`, `kqv_out`, `attn_out`,
  `ffn_norm`, `ffn_gate`, `ffn_swiglu`, `ffn_out`, `l_out`, `result_norm` and `result_output`.
  `inp_embd` and `inp_pos` are not delivered (the graph names the first, the scheduler does not offer
  it; the second does not exist as a row per token).

## [0.10.0] - 2026-10-18

### Added
- **Introspection**: `POST /v1/introspect` returns the raw logits the model computed — argmax, an
  optional top-k, an optional full-vocabulary float row, entropy — for the prompt positions and the
  generated tokens you name, together with the **per-layer activations** copied out of the compute
  graph (`l_out`, `ffn_out`, `attn_out`, `attn_norm`, `ffn_norm`, `result_norm`, `result_output`) for the
  layers you name. The same `introspect` field works on `/completion`. See
  [docs/INTROSPECTION.md](docs/INTROSPECTION.md) and `scripts/ps5lm-introspect.py`.
- **Interventions** in the same request: zero, scale or add a value to one channel (a single FFN
  neuron, an attention-head dimension) or a range, at every position or at one named position. The
  answer reports how many elements each intervention changed, and the prompt cache is dropped after a
  pass that was intervened.
- **The endpoint is off by default**: Settings > Developer > "Logits and activations API" turns it on,
  and it sets `LLAMA_INTROSPECT=1` for the server. `LLAMA_INTROSPECT_MAX_BYTES` caps the activations in
  one answer (256 MiB).

### Verified on a console
- [TESTED-ON-CONSOLE] The 0.10.0 artifact ran on a 13.60 PS5 with MiniCPM5-2B Q8_0 on the GPU
  (ggml-vulkan on RADV, `-c 65536`, server build `b11608-04f7226a2`): logits with entropy at the last
  prompt position in 0.07 s, a full vocabulary row of 130,560 float32, 13 activation tensors of 2048
  floats for layers 0/1/2/12 plus `result_norm`, every prompt position, the chat-template path, and an
  intervention that changed 20,480 elements and moved the argmax with a top-10 KL of 0.915 nats. The
  same token's tensors are byte-identical between batched and step-by-step capture. `docs/CONSOLE.md`
  has the table.

### Changed
- The request feed on Home ignores introspection requests: their answers are raw numbers, and one of
  them can be hundreds of megabytes, which the feed would only parse to show nothing.
- A request that names positions (or intervenes at one) now marks every prompt row as an output row.
  llama.cpp computes the last layer's output and `result_norm` only for rows that asked for logits,
  so without this a per-position capture of `l_out-<last>` and `result_norm` was empty.

## [Unreleased]

### Added
- **`llama-inspect`**, a second payload (and cmake target) that opens the model up for experiments:
  the full logit vector at any position, any graph tensor by name (`ffn_gate-L`, `kqv_out-L`, ...)
  as per-token summaries or raw `float32`, in-place interventions on those tensors (zero, scale,
  add a channel of one neuron or head), a `baseline: true` mode that runs a prompt both ways in one
  call, and greedy generation with per-step logits and activations. One context, one request at a
  time, so a capture is the whole batch and a tensor's token axis is unambiguous
  ([docs/INSPECT.md](docs/INSPECT.md), `scripts/ps5lm_inspect.py`,
  `examples/interpret/neuron_probe.py`). CPU only for now: the GPU belongs to the app.

## [0.9.0] - 2026-10-16

### Added
- A new screen, designed from scratch: **Home** (the model, its speed, and every request and its
  answer streaming as it happens), **Models** (on this PS5, suggested models measured on the console,
  Hugging Face search and downloads), **API** (the endpoints and an example) and **Settings** (with
  the logs under Diagnostics). Manrope type, interface sounds from PocketVibe, and a full-screen
  splash while the app restarts for a model.
- **Decision models**: llama.cpp's `/v1/systemone` (TypeSafe's API, the one Ollaya serves) with d1,
  d1 Omni, Clef, OpenJev and others. The planner gives them a 2048-token batch and a 4k context.
- MiniCPM-V 4.7's image projector, and the other models of llama.cpp 04f7226.
- `/data/PS5LM/env.txt`: driver and ggml switches set before the GPU opens, for measuring.
- **Console** page: temperature, power, GPU and CPU load charted over the last two minutes or the
  last hour, and a table of every reading.
- Home's request feed reads as a chat, and the console can ask itself: Square sends a demo
  request, Triangle opens a keyboard (with Turkish letters) for a message of your own.
- Settings with several choices open a list under the row; Cross picks, Circle cancels.

### Changed
- The app is **PS5LM** under its own title ID, **PPSA99575**, with a new icon and home screen
  background. PS5LM Plus (PPSA99581) stays @abhi245y's app.
- llama.cpp moves to 04f7226; RADV carries three of PS5CEMU-HAR's driver patches (`patches/mesa`),
  among them freed buffers waiting for the GPU.
- Models with an image projector get a 2048-token ubatch, so images are no longer cut to 64 tokens.

### Fixed
- The app opens on Home, not on the model list, and draws its splash before the GPU opens, so a
  restart for a model no longer shows a black screen.
- Home shows the load as it happens, how much of the model is on the GPU, instead of a still
  splash, and offers Demo and Message only once the server answers: requests sent while the model
  loaded were lost.
- One sound for each kind of action, at one pitch: no two actions share a sound by accident.
- A float in any request (`"temperature": 0.7`) stopped the server: the console's `localeconv()`
  reports an empty decimal point. The app now gives the C locale's.
- An image projector llama.cpp cannot read no longer stops the server: the app restarts once and
  loads the model text only.
- `scripts/build-app.sh` fetches the OpenGL SDK through ps5-homebrew-ui's script, creates `build/`,
  and no longer needs ImageMagick.

### Known issues
- A decision model can stop answering after about 55 requests in a row on the console (the server
  waits on the GPU); restart the app or load the model again. Language models are not affected.

## [0.8.3] - 2026-10-13

### Changed
- The app is now called **PS5LM Plus**, to tell it apart from Mert Cobanov's
  original PS5LM payload: on the home screen, in the dashboard header and in
  the console's notifications. The title ID (PPSA99581), the data folder
  (`/data/PS5LM`) and the repository stay as they were, so updating keeps
  settings, models and downloads.
- The dashboard header shows the chat's address without "Chat at http://",
  so it clears the page tabs with any IP address.

## [0.8.2] - 2026-10-13

### Changed
- `contentVersion` in `sce_sys/param.json` now follows the app's version
  (major+1, minor, patch: 0.8.2 is `01.008.002`). It had stayed at
  `01.000.000` since the first release, so stores such as ProsperoStore
  could not tell an installed copy was out of date. `scripts/release-app.sh`
  refuses to package a release whose `contentVersion` doesn't match.
- README: new screenshots from v0.8.1 (Qwen 3.8 27B generating, the library,
  the loaded-model panel) and a shorter text.

## [0.8.1] - 2026-10-13

### Fixed
- The chat server stopped answering after the network dropped: when the
  console's link went down, accept() failed with `SCE_NET_EINACTIVEDISABLED`
  (errno 163, the same `0x804101a3` a dropped download reported), cpp-httplib
  took that as fatal and closed the listening socket, and the model stayed
  loaded with nothing to reach it. llama-server now binds again once a second
  until it works (`patches/0005-listener-survives-network-drops.patch`).
  Checked on the console: the listener came back within a second after each
  of dozens of drops. A reply streaming at the moment of a drop is still cut.

### Added
- README: the web UI with a sample chat.

### Found
- The repeated "Ethernet disconnected" notifications are the console's link
  going down and up (`eth0: link state changed to DOWN` in the kernel log,
  up to eight times in a few seconds), with or without PS5LM running: a cable,
  port or switch problem, not the app.

## [0.8.0] - 2026-10-13

### Added
- Downloads page (the fifth on L1/R1): a queue kept in
  `/data/PS5LM/downloads.json`, each entry with its speed, time left and
  state. Cross pauses or resumes (or tries a failed one again), Square twice
  cancels and deletes the partial file, or clears a finished one.
- Downloads survive a dropped connection, a pause and the app restarting: the
  file is fetched in 32 MiB pieces on two connections, the part done from its
  start is saved with the queue, and a lost connection is retried on its own
  (4, 8, 16, 32, then 60 s apart, up to 20 times in a row). The SHA-256 is
  checked once the file is whole.
- Get models: Square checks whether the file under the cursor fits, and the
  row says CHECKING while its header is read.
- A busy screen (a spinner and what is happening) while a model is switched or
  unloaded, before the restart for "unload and download", and while the
  models are planned again after a context or cache change.
- README rewritten around the app: features, the payloads it needs, controls.
  The payload's README moved to `docs/PAYLOAD.md`.

### Fixed
- A download that lost its connection failed and threw away what it had
  (2.0 of 10.4 GB, `sceHttp2ReadData 0x804101a3`); now it carries on.
- A connection that went quiet could hang a download: requests now time out
  after 30 s without data and are asked again.
- Settings froze the app for seconds on every change, each one reading every
  model's header again. Only the context and cache settings re-plan now, in
  the background behind the busy screen; the others apply at once.

### Measured (download speed on the console)
- The PC on the same network: 19.6 MB/s from Hugging Face on one connection.
- libSceHttp2 (HTTP/2), one connection: 2.8 to 5.9 MB/s, varying more from run
  to run than with any setting. Reads of 256 KiB gave 0.65 MB/s (each read
  waits about half a second), 4 MiB and larger 3 to 6 MB/s. Four requests on
  one template share one connection (3.5 to 6 MB/s); on four templates, made
  one at a time, 2.8 MB/s; on two, 4.2 MB/s. Made in four threads at once,
  the templates crashed the app.
- libSceHttp2 takes at most 4 requests (512 KiB pool); 8 or 16 are refused
  (`0x817b121c`).
- libSceHttp (HTTP/1.1): 0.44 MB/s per connection whatever the read or receive
  block size, adding up with more connections (4: 1.6, 8: 2.5 MB/s, one of
  the eight refused).
- In the app, two pieces at once on their own templates: 7.0 to 9.8 MB/s for
  a 1.9 GiB file (v0.7: 3.8 MB/s), still well under the PC's speed. Going
  further needs sockets and TLS of our own, for a later release.

### Checked on the console
- A 1.9 GiB download: paused (the partial file stopped growing), resumed from
  the saved 128 MiB, closed and relaunched mid-download (carried on from
  905 MiB), finished with its SHA-256 matching.
- Get models during a download: search, listing and fit checks work.
- The busy screen on a context change; the library re-planned behind it.
- Payloads stayed up throughout.

## [0.7.0] - 2026-10-10

### Added
- Downloads check free space first: the file and 1 GiB to spare must fit on
  the drive. When it does not, a dialog offers the drive with the most room
  for this one download; the download itself refuses too, for the
  after-restart path.
- M.2 and every mounted USB drive: Download to in Settings steps through the
  drives that are mounted (internal, `/mnt/ext0`, `/mnt/usb0..3`), the
  STORAGE tile and the usage details list each one, and the library groups
  models by drive. Models in `PS5LM/models` on the M.2 drive are found too.
- Before a download while a model holds the memory (under 2 GiB free), a
  dialog offers to unload it first: the app restarts without the model and
  the download starts on its own.

### Fixed
- Storage figures were made up: a title's `statvfs` is a stub that reports
  64 GiB with 16 GiB free for every path, and `statfs` is not exported to
  titles. Free and total space now come from ps5-exporter (bundled since
  v0.4.0), which reads the kernel's; internal storage is `/user`. Without the
  exporter the space shows as unknown and does not block a download.
- Get models opened after a restart now lists the popular repositories.
- A download without a checksum survives the unload-and-download restart.

### Checked on the console
- Real space: internal 166.7 of 627.6 GiB free, USB 461 of 477 GiB, as
  ps5-exporter reports them; the drive's 33 MB second partition is left out.
- A 500 GiB job refused on internal storage; a download to the USB drive;
  Download to stepping between internal and USB, saved; unload and download
  (SmolLM2 Q2_K); the scratch size warning and Clear.
- No M.2 drive is fitted, so that path is checked in the preview only.

## [0.6.0] - 2026-10-10

### Added
- Get models page (the fourth on L1/R1): Hugging Face's GGUF repositories,
  most downloaded or by family (L2/R2), each file's size and whether it fits,
  planned from its first 16 MiB, and downloads to internal storage or USB
  (Settings), SHA-256 checked, added to the library when done. HTTPS through
  the system's own libSceHttp2 and libSceSsl. Checked on the console:
  SmolLM2-135M F16 (271 MB) at 3.4 MB/s.
- Chat tools: the browser chat's model can read, write, edit and search files
  (llama-server's built-in tools), confined to `/data/PS5LM/scratch` by
  `patches/0003-tools-root.patch`. Settings has a switch, a size warning
  (default 2 GiB) and a Clear action.
- Images: a vision model's `mmproj` file beside it is loaded with it
  (`--mmproj`), and the planner counts its memory.
- The planner can plan from the first bytes of a GGUF (a range request), the
  base of the model market's fit column.

### Measured
- Writes from the app: stdio 18.5 MB/s, write() 232 MB/s on /data (USB: 293
  and 621 MB/s). Downloads use write().
- Chat tools on the console: a write to /data/homebrew lands in the scratch
  folder; relative paths start there. Gemma 4 E4B with its projector
  described a test image correctly.

## [0.5.0] - 2026-10-10

### Added
- `docs/CLI.md`: opencode, codex and Claude Code against the model on the
  console, each checked on a small edit with Qwen3.8-27B UD-IQ2_XXS.

### Fixed
- Claude Code got HTTP 500 on every request: it sends system messages in the
  middle of a conversation and Qwen's template refuses them.
  `patches/0004` folds them into the first system message.

## [0.4.0] - 2026-10-10

### Added
- POWER card: the SoC's power (CPU, GPU and memory together), its history,
  energy used this session and joules per generated token; expanded, min,
  average and max, fan duty and every temperature sensor. The reading is
  `sceKernelGetSocPowerConsumption`'s low 32 bits in milliwatts (22 W idle,
  86 W while the 27B generates), through ps5-exporter.
- USAGE card: GPU and CPU on one chart; expanded, GPU, CPU, memory, storage
  and the models on disk.

### Changed
- USAGE replaces the GPU and CPU tiles; THERMALS shows the hottest sensor.

### Fixed
- Speed and GPU use during a long reply: llama-server's counters move only
  when a request ends, so the live figures come from the slot's token count.

## [0.3.0] - 2026-10-10

### Added
- Pages on L1/R1: Dashboard, Settings and Logs, with tabs in the header.
- Settings page (rail and rows after the kit's Control Room design): load a
  model at launch and which one, the planner's longest context and KV cache
  type, interface sounds; saved to `/data/PS5LM/settings.json`.
- Logs page: `app.log` and `llama.log`, scrollable, with an errors-only filter.
- Model details on Cross over the Model tile: every llama-server argument, the
  plan, cache and preset; Triangle opens the library.
- THERMALS tile (SoC and CPU temperature, fan duty), STORAGE tile (free space
  on /data and USB), and the CPU clock. A title may not read the sensors
  (`0x80020002`), so the app bundles Marice's ps5-exporter (GPL-3.0, pinned
  v0.2.0) and sends it to elfldr when it is not running.
- `scripts/ps5lm-app.sh press <button>`: drive the screens from a PC.

### Changed
- One MEMORY tile replaces MEMORY and GPU MEMORY (the GPU and CPU share one
  pool); CONTEXT takes in the session counts.

## [0.2.0] - 2026-10-10

First release of the native app (title PPSA99581), from this fork.

### Added
- Native PS5 app running llama-server on the GPU through ggml-vulkan and RADV,
  with an 11.44 GiB unified device heap.
- Dashboard on the TV (ps5-homebrew-ui): generation speed, GPU busy, CPU use,
  memory pool, GPU memory split, session, context and model tiles.
- Model library with internal storage and USB drive sections; models load
  straight from USB.
- Planner that picks context length and KV cache type for the memory free.
- Sampling presets for Qwen 3.5/3.6/3.8, Gemma 4, gpt-oss, Granite, Nemotron,
  Mistral and Llama.
- Model switching by restarting the app (in-place unload leaks GPU memory).
- `scripts/ps5lm-app.sh` to drive the app from a PC, and the version on the
  dashboard and in the log.

### Fixed
- Unloading during a reply no longer hangs the app: it waits at most 10 s for
  llama-server before restarting.

### Changed
- A plain launch loads no model and opens the library.
