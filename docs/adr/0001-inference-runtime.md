# ADR-001: Inference runtime for stem separation

- **Status:** Proposed
- **Date:** 2026-11-12
- **Deciders:** Paramix maintainers
- **Supersedes:** none
- **Source material:** [`docs/paramix-stem-beatgrid-ui-plan.md`](../paramix-stem-beatgrid-ui-plan.md) §5.3, §5.4, §5.9, §6.3, §6.7, §6.8, §8.2, §9, §10; [`docs/beatgrid-audit.md`](../beatgrid-audit.md) §1, §5
- **Related:** ADR-002 (stem cache format), ADR-003 (beatgrid format), ADR-005 (model distribution)

---

## Decision summary

1. **ONNX Runtime (ORT)** is the inference runtime for stem separation: MIT licensed, C++ API, and
   execution providers (EPs) for every shipping target.
2. Inference sits behind **`StemSeparatorBackend`** (`src/stems/stemseparatorbackend.h`), whose
   header names no runtime. `StemSeparation` (`src/stems/stemseparation.h`) owns windowing and
   cross-fading; the backend owns one inference window.
3. **CPU is always available.** CoreML, DirectML and CUDA are optional EPs; a GPU provider that
   fails to initialize degrades the job to CPU rather than failing it.
4. The build flag is **`STEM_SEPARATION`**, tri-state `AUTO` / `ON` / `OFF`, default `AUTO`
   (ON when ORT is found). `OFF` produces a fully functional build with no ORT symbols.
5. **One session per worker**, intra-op threads bounded, inter-op fixed at one, workers at low
   priority. Separation never runs on, allocates on, or locks the audio thread.
6. **No model weights are bundled.** ORT is a library; weights are downloaded per ADR-005.
7. The **cache key excludes the runtime version and the device** (ADR-002 §2.3): an update within
   tolerance is not an invalidation, while a change beyond tolerance forces a model version or
   separation-parameter bump.
8. `demucs.cpp` remains the documented **compile-time fallback** behind the same interface.

---

## 1. Context

### 1.1 What already exists

There is **no inference runtime in the tree**: `docs/beatgrid-audit.md` §1 records "ML inference
runtime — Absent" and "Bundled model precedent — Absent", and `docs/agent-context.md` lists
`src/stems/` as not started. `KEYFINDER` in `CMakeLists.txt` shows the pattern for an optional
native dependency: an option, a versioned `find_package()`, an `ExternalProject_Add()` fallback,
guarded sources and a `__KEYFINDER__` capability define.

Two tree facts constrain the link. The tree links its own protobuf (`src/proto/CMakeLists.txt`
prefers `protobuf::libprotobuf-lite`), while ORT embeds its own — the ODR hazard in
`docs/beatgrid-audit.md` §5. And macOS is sandboxed and notarized:
`packaging/macos/Mixxx.entitlements` grants network and file access, and
`packaging/macos/sign_notarize_staple.sh` signs the release, so a new dylib must be signed inside the
bundle.

The interface is already in the tree. `StemSeparatorBackend` declares `sampleRate()`,
`windowFrames()`, `overlapFrames()` and `processWindow(const CSAMPLE*, CSAMPLE*)`, includes only
`audio/types.h` and `util/types.h`, and states that backends run only on a background worker and may
keep a session between calls. `StemSeparation::separate` streams two windows plus an overlap
accumulator and polls a cancel callback between windows.

### 1.2 What is missing

- A session lifecycle: graph loading, EP selection, thread and memory configuration.
- A concrete backend behind the interface, plus a factory that reports whether a runtime and a
  model are available, so a build without stems hides actions instead of failing at runtime.
- Measured numbers. The plan's figures (§5.3, §8.2) come from published benchmarks, not this tree;
  they are targets here, not evidence.

### 1.3 Constraints

- **GPL-2.0 compatibility.** Every linked library must be GPLv2-compatible (plan §2, §10 item 1).
  ORT is MIT and compatible.
- **Real-time audio.** Separation, disk I/O and model loading stay off the engine thread (plan §2).
  Priority alone did not prevent starvation (`src/engine/sidechain/enginesidechain.cpp:31-37`), so
  thread count must be bounded too.
- **Four targets.** Windows x64, Linux x64, macOS arm64 and macOS x86_64 (plan §11, decision 6).
- **No weights in the repository.** `tools/paramix_verify.sh:398` rejects tracked `models/*`.
- **Memory budget.** Under 1.5 GB RSS with two separation workers (plan §8.2).

### 1.4 Decisions already made

Separation is **offline and background**, not real-time (ADR-002 §1.4; plan §5.5). The default model
is `htdemucs_ft`, with `htdemucs` as the low-end fallback (plan §11, decision 1). GPU acceleration is
**auto-enabled with a CPU opt-out** (decision 12). Models are **download-only**, with ADR-005 owning
distribution (§5.9), and `demucs.cpp` is the documented CPU-only fallback (§5.4).

---

## 2. Decision

### 2.1 Runtime and interface

**ONNX Runtime**, for reasons specific to this program. It is **MIT** licensed, compatible with
GPL-2.0 and the same family as the model code (plan §10 item 1). Its **C++ API** (`Ort::Env`,
`Ort::Session`, `Ort::SessionOptions`) needs no Python runtime, sidecar process or C shim. Its
**EPs map one-to-one onto the targets** — CoreML (macOS), DirectML and CUDA (Windows), CUDA (Linux),
CPU everywhere. It exposes **thread and memory control** per session (intra-op and inter-op counts,
sequential execution, the CPU arena, the spin setting), which §2.4 depends on. And the **export path
already exists**: the toolchain emits ONNX (`demucs-onnx`, MIT, plan Appendix C), with xLights as a
shipping C++ precedent on CoreML/DirectML/CPU (plan §5.3).

**Interface boundary.** The backend header is runtime-agnostic by construction: it names no runtime
and includes only audio types. `StemSeparation` owns the whole-track loop — windowing, overlap-add,
tail padding and trimming, progress and cancellation between windows. `StemSeparatorBackend` owns
exactly one window: `processWindow()` takes interleaved stereo of `windowFrames() * 2` samples and
writes eight-channel output of `windowFrames() * 8` samples, drums/bass/other/vocals.
`OnnxStemSeparatorBackend` owns the session; `NullStemSeparatorBackend` reports unavailable;
`MockStemSeparatorBackend` returns deterministic stems for tests; `DemucsCppStemSeparatorBackend` is
the fallback. Session creation, EP selection and the model path belong to the concrete backend and
its factory, which returns a backend or an error string and is the one place that answers "is
separation available?", keeping the cache, queue, UI and tests free of ORT.

### 2.2 Build integration

`STEM_SEPARATION` is a cache variable with three values, not a boolean:

| Value | Behavior |
|---|---|
| `AUTO` (default) | Probe. Found: compile the ORT backend and define `__STEM_SEPARATION__`. Not found: status message, build without stems, no error. |
| `ON` | Require ORT. Configure fails with an actionable message if it is missing. Used by release packaging and CI. |
| `OFF` | No probe, no ORT sources, no define. Used by distro and Flatpak builds, offline builds and the "no stems" CI job. |

`AUTO` is the requested "default ON when ONNX Runtime is found" without turning a missing optional
dependency into a configure failure on a contributor's machine; packaging sets `ON` so a missing
runtime cannot ship silently. The plan's working name `STEMS` (§6.8) is superseded by
`STEM_SEPARATION`; the GPU sub-option keeps the plan's intent as `STEM_SEPARATION_GPU`.

**Discovery.** A new `cmake/Findonnxruntime.cmake` searches `STEM_SEPARATION_ORT_ROOT` first,
accepts a config package (`find_package(onnxruntime CONFIG QUIET)`) when one exists, and otherwise
locates `onnxruntime_cxx_api.h` and the `onnxruntime` library with `find_path` / `find_library`,
honoring `CMAKE_PREFIX_PATH`; it exports `onnxruntime::onnxruntime`. v1 does **not** download ORT at
configure time: the audit's packaging table (`docs/beatgrid-audit.md` §5) shows that path is the
expensive part, and offline CI is worth more. A source build for Flatpak or universal macOS, if the
spike requires it, follows the `libkeyfinder` `ExternalProject_Add` pattern.

**Sources and define.** `src/stems/stemseparation.cpp` is already in the `mixxx-lib` source list.
Only the ORT implementation is conditional: `onnxstemseparatorbackend.cpp` is added with
`target_sources()` inside `if(STEM_SEPARATION)`, and the capability define mirrors `__KEYFINDER__`.

**What must not break when ORT is absent:** all five CI jobs, including macOS x86_64 with `QML=OFF`
(plan §3.4), must build warning-free on all four targets; distro and Flatpak builds must still work;
model-dependent tests must use the mock backend; no widely included header may include
`onnxruntime_cxx_api.h`; `OFF` must not probe the network or filesystem; and the UI must ask the
factory, so a build without stems shows "separation unavailable" instead of a dead action.

### 2.3 Execution providers

| Platform | Default | Optional | Notes |
|---|---|---|---|
| macOS arm64 | CoreML | CPU | CoreML partitions; unsupported nodes run on CPU |
| macOS x86_64 | CPU | CoreML | CoreML on Intel is unproven here; the spike decides |
| Windows x64 | DirectML | CUDA, CPU | DirectML avoids a hard CUDA dependency (plan §6.8) |
| Linux x64 | CUDA | CPU | CUDA optional; a CPU-only build is valid |
| Linux arm64 | CPU | none in v1 | Subject to ORT/EP availability (plan §6.8) |
| Any | CPU | — | Always present, always the last resort |

`[Stems],ExecutionProvider` accepts `auto` (default), `cpu`, `coreml`, `dml` and `cuda`, which
implements plan decision 12. `auto` resolves to the platform default, but only after querying the
runtime's actual provider list; a provider is never assumed to be compiled into the binary.

**Failure degradation.** Initialization is three fallible steps: append the EP, create the session,
run one warm-up inference on a silent window. If any step throws, log once at warning with the
provider and the ORT message, mark that provider unavailable for the process lifetime, and retry on
CPU. If the CPU session also fails, fail the job with a specific error: that is a broken
installation, not a GPU problem. If a later `Run` fails mid-job, re-run only that window on CPU;
windows are independent and `StemSeparation` owns the cross-fade, so no audio is lost. A provider is
never retried per window, so timing and memory stay predictable. The effective provider goes into
`meta.json` (`model.runtime`, `model.runtime_version`, `model.device`, ADR-002 §2.4) for diagnostics,
but is **not** in the cache key (§2.6).

"GPU enabled" does not mean "all nodes on GPU": ORT assigns unsupported nodes to CPU. The spike
reports the assignment with ORT profiling, which production leaves off.

### 2.4 Sessions and threading

**One session per worker.** A session is created when a worker starts, reused for every job it
takes, and destroyed after an idle timeout (proposed: five minutes) so its memory returns to the OS.
Session creation takes seconds, especially with CoreML graph compilation, so it never happens per
window or per track.

**Intra-op threads are bounded.** `[Stems],MaxThreads` defaults to
`min(4, max(1, hardware_concurrency() / 2))`, clamped to `[1, hardware_concurrency()]`. With at most
two workers (the memory target assumes two), the total leaves cores for the engine, the GUI and the
waveform. The default is a tuning number the acceptance soak confirms; it is a setting because a
4-core laptop and a 16-core desktop need different answers.

**Inter-op threads are one**, with `ORT_SEQUENTIAL` set explicitly: the graph is a chain, and
parallelism belongs across windows and jobs. **Spinning is disabled** with
`session.intra_op.allow_spinning=0`; ORT's pool spins briefly before yielding, the wrong behavior
next to an audio callback. Workers run at `QThread::LowPriority`, matching
`src/analyzer/analyzerthread.cpp:79`.

**Memory and allocation.** The CPU arena stays enabled for the session's lifetime, because repeated
same-shape runs are the normal case; the idle timeout returns memory. If the soak exceeds 1.5 GB, the
next step is `DisableCpuMemArena()` at a measured RTF cost. Separation is offline (ADR-002 §1.4): the
audio callback never calls ORT, loads a model, allocates a window buffer or takes a separation lock.
Buffers and output stems are allocated on the worker for the duration of a job. The only RT-adjacent
path is reading a finished cache file through `SoundSourceStemCache`, the existing `SoundSource`
decode path (ADR-002 §2.7), which does not involve this runtime.

### 2.5 Model contract

The backend speaks one tensor contract; `StemSeparation` speaks interleaved buffers.

| Property | Value |
|---|---|
| Input | float32, `[1, 2, windowFrames]` (batch, stereo, samples) |
| Output | float32, `[1, 4, 2, windowFrames]` (batch, stems, channels, samples) |
| Batch | Fixed at 1 in v1 |
| Sample rate | **44100**, fixed by the model; `sampleRate()` reports it |
| Channels | 2; mono sources are duplicated before inference |
| Stem order | From the manifest; default drums, bass, other, vocals (ADR-002 §2.2) |
| Normalization | None in the backend; the cache writer applies the ADR-002 scalar gain |

**Chunked windows.** HTDemucs is a fixed-segment model; the graph does not accept arbitrary
lengths. The backend reports `windowFrames()` and `overlapFrames()`, and `StemSeparation` builds the
sequence from them. At 44.1 kHz the reference segment is 7.8 s, about 343,980 frames (plan §6.3);
ADR-002 §2.4's `window_frames: 344000` is illustrative, the manifest is authoritative, and the
actual value goes into the pipeline key. Default overlap is 0.25, so the hop is three quarters of a
window; the final window is zero-padded and the output trimmed to the source frame count, keeping
the cache a drop-in replacement (ADR-002 §2.2). The cross-fade weighting is a separation parameter
in the key (ADR-002 §2.3) because changing it changes output.

**Opset and quantization.** The manifest records the ONNX opset, the IR version and the weight
dtype; the runtime must support them, with no conversion at load time. fp32 is the correctness
baseline everywhere. fp16 is permitted where the graph is fp16 and the EP handles it natively
(CoreML, DirectML, CUDA); on the CPU EP it is usually cast to fp32 and can be slower, so the spike
measures it first. int8 is not a default: it is considered only if measured SDR loss against fp32
stays within the parity target in §6, and it never replaces the fp32 fallback. Opset and dtype are
model properties; changing either is a model version change and therefore a new cache key.

**Cancellation and errors.** Cancellation is polled between windows by `StemSeparation`, because ORT
cannot interrupt a running graph. A cancelled job discards its partial output; the cache writer
never sees an incomplete buffer (ADR-002 §2.6). `processWindow()` returns false on failure and no
ORT exception escapes the backend.

### 2.6 Determinism and reproducibility

Within one process, session, device and thread count, the CPU EP is effectively deterministic. GPU
kernels need not be reproducible, and thread count changes floating-point reduction order, so tests
compare with a tolerance.

**Why the key excludes the runtime version and the device.** ADR-002 §2.3 decides this, and it
stays: an ORT update, a driver update or moving a library between machines must not invalidate a
terabyte of cache. Re-separating because a patch release changed the last bit of a float is hours of
user time for an inaudible difference, and the cache is local and disposable (ADR-002 §2.8). The
artifact is "the model's output within tolerance", not "a bit-exact function of the binary". One
`pipeline_key` can therefore name slightly different FLAC bytes on two machines; `meta.json` records
the runtime version and device so a support report can explain the difference, but lookup ignores
them.

**What must change if a runtime update alters output.** If the change is audible or exceeds the
parity tolerance in §6, one of the two key inputs must change. Preferred: bump `model_version` in
the manifest, which also changes the download and honestly signals that the artifact changed.
Alternative: change a `separation` parameter (window, overlap, shifts, weighting, normalize) if the
difference comes from how the model is called rather than from the weights. `cache_format_version`
is **not** the lever: it describes the container, not the audio. A runtime change that alters the
model *contract* (opset, input length, output layout) is a model change by definition.

### 2.7 Packaging and licensing

**What ships.** The ORT shared library per platform: `onnxruntime.dll`, `libonnxruntime.so`,
`libonnxruntime.dylib`. The plan estimates 30–60 MB per platform (§5.4); the actual size is measured
in the spike and recorded in the release notes. The model manifest is a small JSON file in `res/`.
**No weights ship.**

**Dynamic linking is a decision.** Static linking would put two protobuf implementations in one
binary, the ODR hazard in `docs/beatgrid-audit.md` §5. The prebuilt shared library hides its
internal symbols; the spike includes a link test and an exported-symbol check proving that
`mixxx-proto` and ORT coexist. If that fails, the fallback is a source build with a shared,
symbol-hidden configuration, not accepting the collision.

**macOS.** The dylibs go inside the bundle, are signed with the app identity under the hardened
runtime, and are notarized with the app by `packaging/macos/sign_notarize_staple.sh`. The existing
entitlements already grant the App Sandbox, network access and user-selected file access, covering
the model download and "install from file"; CoreML needs no new entitlement. Upstream ORT ships
per-architecture binaries, so a universal build requires a `lipo` of the two slices or a source
build with `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. The plan assumes universal dylibs (§6.8); the
spike must prove the route before packaging commits to it.

**Windows, Linux, licensing, updates.** Authenticode-sign the DLL; DirectML is optional and CUDA is
not a hard dependency, so a machine without either still runs on CPU (plan §6.8). The AppImage
bundles the `.so`; Flatpak builds ORT as a module or vendors a pinned binary, and a CPU-only distro
build with `STEM_SEPARATION=OFF` is supported. ORT is MIT (plan §10 item 1): add it to the NOTICE
and to `docs/third-party-models.md`, which ADR-005 owns, and ship its license and third-party
notices. ORT is a code dependency; weight licenses remain ADR-005's problem. The ORT version is
pinned per app release and recorded in `meta.json`; a runtime bump is a normal app update, not a
model update, and does not invalidate the cache (§2.6).

---

## 3. Alternatives considered

### 3.1 NCNN

Rejected for v1. BSD-3 and small, with good ARM/Vulkan CPU performance, but fewer transformer
operators, a conversion step no HTDemucs export targets, and no CoreML or DirectML equivalent. It is
the natural choice if Android or iOS is added, which is out of scope (plan §6.8).

### 3.2 LibTorch

Rejected. Hundreds of megabytes per platform, tight compiler and C++ runtime coupling, and a second
tensor/ABI surface next to Qt. The plan calls it overkill (§5.4); nothing here needs TorchScript,
autograd or training.

### 3.3 Cloud inference

Rejected. It contradicts offline and private DJ use (plan §2, §9), adds network latency and cost to
a background job expected to run on a plane, and makes the cache depend on a service.

### 3.4 TensorFlow Lite, or Spleeter's runtime

Rejected on license and quality. TensorFlow/TFLite are Apache-2.0, which the plan identifies as
incompatible with GPL-2.0 (§2, §10), and Spleeter's quality is below the bar (§5.3).

### 3.5 `demucs.cpp` only (no ORT)

Rejected as the default, retained as the fallback. MIT, CPU-only, no ORT, but roughly four minutes
per four-minute track with four threads (plan §5.3), which misses the CPU target and offers no GPU
path. It stays behind the interface for builds with `STEM_SEPARATION=OFF` that still want
separation, platforms where ORT cannot be packaged, and a diagnostic comparison. Because the
interface is shared, choosing it is a build and manifest decision, not a rewrite.

### 3.6 Static ORT, vendoring, or a runtime-loaded plugin

Rejected as the default. Static linking collides with `mixxx-proto`'s protobuf (§2.7), and vendoring
adds a multi-gigabyte build to every CI job; a source build remains the documented path for Flatpak
and universal macOS. A `dlopen` plugin was considered and deferred: it would let a distro ship one
binary and add ORT later, but it moves link errors to the user's machine, complicates macOS signing,
and turns "no ORT" into a runtime failure instead of a compile-time capability.

### 3.7 A custom ggml or hand-written inference kernel

Rejected. `demucs.cpp` already covers the small CPU-only niche, and maintaining a transformer
runtime is a project of its own. The program's value is the DJ integration, not a kernel library.

---

## 4. Consequences

**Positive**

- One runtime covers all four targets with a maintained upstream and a permissive license.
- The interface keeps the cache, queue, UI and tests independent of ORT, so ADR-002 can land and
  be tested with a mock backend before a model exists.
- CPU always works, so a GPU failure is a performance event, not a failed job.
- Bounded threads and disabled spinning protect the audio thread by construction, and the cache
  survives runtime and driver updates.
- The fallback is a build configuration, not a fork.

**Negative**

- Roughly 30–60 MB per platform added to every installer, plus signing and notarization per
  architecture.
- A new optional dependency doubles the build matrix for stems; the "without" job must stay green
  or the optionality is fiction.
- Protobuf coexistence must be proven and watched; a future ORT release could change its symbol
  visibility.
- GPU results are not bit-reproducible, so tests and support need tolerances and device metadata.
- Session memory is significant, and the idle timeout trades RSS against reload latency.

**Risks and mitigations**

| Risk | Mitigation |
|---|---|
| ORT's protobuf collides with `mixxx-proto` | Dynamic link, hidden symbols, link test and symbol check (§2.7) |
| GPU provider initializes but is slower than CPU | Warm-up inference, node-assignment profiling, `ExecutionProvider=cpu` |
| Separation starves the audio thread | Bounded intra-op threads, spinning off, low-priority workers, xrun soak (§6) |
| macOS universal build cannot use prebuilt ORT | `lipo` or source build; decided by the spike before packaging |
| Missing ORT breaks a distro build | Tri-state flag, `AUTO` default, `OFF` path in CI, no ORT headers in public headers (§2.2) |
| Model update silently reuses old cache | Model version is in `pipeline_key` (ADR-002 §2.3); runtime version is not (§2.6) |
| GPU crash mid-job loses work | Per-window CPU retry, partial output never committed (ADR-002 §2.6) |
| Bundle size exceeds the installer budget | Measured per platform; CPU-only or stripped builds remain possible |
| A runtime update changes audio audibly | Parity tolerance in §6; bump model version or separation parameters |

---

## 5. Implementation order

The first step is a hard gate. Do not freeze packaging or the cache around an unproven runtime.

1. **Spike (Phase 0).** Export or obtain `htdemucs_ft` in ONNX; run it on macOS arm64 CPU and
   CoreML, and on Windows x64 CPU and DirectML (Linux CPU if available). Measure parity, RTF, peak
   RSS and bundle size. Prove ORT and `mixxx-proto` link together and that a universal macOS build
   is possible. Every number in §6 is filled in here.
2. **Interface and build plumbing.** The ORT backend factory, the null and mock backends,
   `cmake/Findonnxruntime.cmake`, the `STEM_SEPARATION` tri-state, the capability query, and a
   green build with and without ORT on all four targets.
3. **CPU backend.** Session lifecycle, window conversion, cancellation, error mapping, `meta.json`
   device fields. `StemSeparation` already owns windowing and cross-fade.
4. **Execution providers and threading.** EP selection, the `[Stems],ExecutionProvider` setting,
   warm-up, process-lifetime disable on failure, per-window CPU retry, bounded intra-op threads,
   spinning off, low-priority workers, idle session teardown.
5. **Packaging.** Bundle, sign and notarize per platform; add the with/without-stems CI jobs; pin
   the ORT version; update NOTICE and `docs/third-party-models.md`.
6. **Integration and benchmarks.** Wire the backend into `StemService` and the cache writer
   (ADR-002), then the status UI (ADR-004); add a non-gating RTF/RSS job per platform and a gating
   parity test on a fixed fixture. None of these steps may depend on ORT types.

---

## 6. Acceptance

Acceptance is gated on the Phase 0 spike. The table states the metric and target; **Measured** is
intentionally empty until the spike runs. No number below is a claim about this tree.

| Metric | Target | Measured |
|---|---|---|
| Parity vs the reference PyTorch implementation, per-stem SDR | Within 0.1 dB of the reference on the fixed fixture | to be measured |
| Parity vs the reference, max absolute sample error | ≤ 1e-3 (the export tool's advertised parity) | to be measured |
| Real-time factor, M-series CPU, `htdemucs_ft` bag | ≤ 150 s per 4-minute track (RTF ≤ 0.63), plan §8.2 | to be measured |
| Real-time factor, GPU provider (CoreML; DirectML or CUDA) | ≤ 30 s per 4-minute track (RTF ≤ 0.125), plan §8.2 | to be measured |
| Peak RSS, two separation workers | < 1.5 GB, plan §8.2 | to be measured |
| Audio xruns, 2-hour soak, 4 decks, 128-frame buffer, two workers | 0, plan §8.2 | to be measured |
| ORT bundle size per platform | ≤ 60 MB, plan §5.4 | to be measured |
| `htdemucs_ft` download size | About 630 MB fp16, plan decision 1 | to be measured |

Acceptance also means:

- **Build matrix.** With and without ORT on all four targets; `STEM_SEPARATION=OFF` produces no
  ORT symbols and a working null backend.
- **Fallback.** A forced provider-initialization failure completes the job on CPU, logs once, and
  records the fallback in `meta.json`; a forced mid-job `Run` failure re-runs only that window.
- **Key stability.** A recorded `pipeline_key` is unchanged after a simulated runtime version and
  device change (ADR-002 §2.3), and changes after a model version or separation-parameter change.
- **No RT regression and no weights.** The soak shows no separation work, allocation or lock on
  the audio thread, and zero xruns; `tools/paramix_verify.sh` still passes with only the manifest
  added.

If a target is missed, this ADR is reopened rather than silently relaxed. The documented fallbacks
are a CPU-only release with `demucs.cpp`, or a narrower platform matrix while the GPU path is fixed.
