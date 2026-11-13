# Handoff: stem integration

**Date:** 2026-11-13
**Repository:** `C:\Users\Bradl\paramix` (GitHub `christianstuart/paramix`)
**Purpose:** continue the Paramix stem work in a fresh chat without replaying the
previous conversation.

Read this first, then:

- `docs/stem-file-export.md` - the new native stem set (FLAC/ALAC/AAC).
- `docs/stems-onnx.md` - the ONNX Runtime backend and the local model setup.
- `docs/adr/0001-inference-runtime.md`, `0002-stem-cache.md`,
  `0003-grid-format.md`.
- `docs/parallel-work.md` - coordination rules and the current backlog.
- `AGENTS.md` - repository rules (real-time audio, branch naming, no direct
  pushes to `main`).

---

## 1. Current state (2026-11-13, post-merge)

The implementation stack is **merged to `main`** by squash merge. All required
checks were green before each merge. `main` is at `1cd8221` at the time of
writing.

| PR | Branch | What it contains | Status |
|---|---|---|---|
| [#17](https://github.com/christianstuart/paramix/pull/17) | `bradley/stems1-demucs-stft` | Host HTDemucs STFT/iSTFT (CaC boundary), 10 tests | Merged (`bd63846`) |
| [#18](https://github.com/christianstuart/paramix/pull/18) | `bradley/stems1-dsp-postprocess` | Normalization, bag averaging, shared limiter, `demucsReconstruct`; 27 tests | Merged (`9852835`, rebased after #17) |
| [#19](https://github.com/christianstuart/paramix/pull/19) | `bradley/stems2-build-option` | `STEM_SEPARATION` AUTO/ON/OFF, ONNX Runtime discovery, vendored demucs-onnx runtime, backend + factory; 7 tests | Merged (`8b0f49c`) |
| [#20](https://github.com/christianstuart/paramix/pull/20) | `bradley/stems3-stem-files` | Native stem set export: FLAC/ALAC/AAC + sidecar; 10 tests | Merged (`7cda9ed`) |
| [#21](https://github.com/christianstuart/paramix/pull/21) | `bradley/docs-stems-status` | Backlog, Traktor correction, this handoff | Merged (`1cd8221`) |

The feature branches remain on the remote but are no longer needed. The
placeholder `STEM_SEPARATION` option and the ORT backend are now in `main`.

**Program board:** <https://github.com/users/christianstuart/projects/2>

| Card | Status |
|---|---|
| Stems 1 - host STFT/iSTFT | Done |
| Stems 2 - ONNX Runtime backend | In progress |
| Stems 3 - native stem set export | In progress |
| Stems 4 - background queue/service | Todo |

Item IDs (for `gh project item-edit`): Stems 1 `PVTI_lAHOBD3Hks4BrZB2zhIXi6s`,
Stems 2 `PVTI_lAHOBD3Hks4BrZB2zhIXjDI`, Stems 3 `PVTI_lAHOBD3Hks4BrZB2zhIXjJ0`,
Stems 4 `PVTI_lAHOBD3Hks4BrZB2zhIXjP8`. Project ID
`PVT_kwHOBD3Hks4BrZB2`.

---

## 2. Architecture snapshot

- `StemSeparatorBackend` / `StemSeparation` (merged in #13) are the generic
  windowed backend seam. They are still used by their own tests.
- `lib/demucs-onnx/` is a vendored copy of the **MIT header-only** runtime from
  `HRSadeghi/demucs-onnx`, commit `757ab9f`. `demucs::Separator` owns the
  whole-track loop: chunking, track-level normalization, overlap-add and bag
  blending.
- `DemucsOnnxSeparator` (`#19`) wraps `demucs::Separator`, converts its planar
  `[sources][channels][count]` output to Paramix's interleaved eight channels
  (drums/bass/other/vocals), and exposes a factory that returns null plus a
  reason when the build has no ONNX Runtime.
- `demucsstft`, `demucsnormalization`, `demucsbag`, `demucslimiter`
  (`#17`/`#18`) are the model-agnostic DSP. The ONNX wrapper currently uses the
  vendored runtime's own STFT; the two implementations coexist. Decide later
  whether the standalone DSP is used by another backend or retired.
- `stemfilewriter` (`#20`) writes four stereo files plus `stem-set.json` in
  FLAC, ALAC or AAC. It is **not yet called** by a service or the UI.
- The playback cache in ADR-002 (one 8-channel FLAC per track) is **not
  implemented yet**. The `#20` export is a separate user-facing artifact; the
  cache remains the planned playback path.

---

## 3. Local environment and commands

**Windows x64, Visual Studio 2022 Build Tools, Ninja, Qt 6.10.**

Build from the MSVC developer environment; CMake is configured in `build/`:

```powershell
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && cmake --build C:\Users\Bradl\paramix\build --target mixxx-test"
```

Run a suite:

```powershell
cd C:\Users\Bradl\paramix
$env:PATH = "C:\Users\Bradl\paramix\build;$env:PATH"
.\build\mixxx-test.exe --gtest_filter=StemFile*:StemSeparationTest.*
```

ONNX Runtime backend (only on the `#19` branch):

```powershell
cmake -S . -B build `
  -DSTEM_SEPARATION=ON `
  -DSTEM_SEPARATION_ORT_ROOT=C:/Users/Bradl/paramix/buildenv/onnxruntime-win-x64-1.32.0/onnxruntime-win-x64-1.32.0
cmake --build build --target mixxx-test

py tools\make_test_demucs_onnx.py --out-dir build\testmodel
$env:PARAMIX_TEST_DEMUCS_ONNX = "C:\Users\Bradl\paramix\build\testmodel\testdemucs.onnx"
.\build\mixxx-test.exe --gtest_filter=DemucsOnnxSeparatorTest.*
```

Real model spike (gitignored, on this machine):

```powershell
$env:PARAMIX_TEST_DEMUCS_ONNX = "C:\Users\Bradl\paramix\models\htdemucs.onnx"
.\build\mixxx-test.exe --gtest_filter=DemucsOnnxSeparatorTest.SeparatesTinyModelAndReportsProgress
```

The real graph is the demucs-onnx `htdemucs.onnx` (160.8 MB) plus
`htdemucs.json`; one padded 7.8 s window separates in about 6 s on the CPU
provider. `models/`, `buildenv/` and `build/` are gitignored.

**Important:** `STEM_SEPARATION` only exists on the `#19` branch. On `main` the
variable is ignored. Reconfigure after switching branches if the build cache was
last configured with `ON` and ORT is not present.

---

## 4. What is done

### Stems 1 - host transform and DSP (`#17`, `#18`)

- `src/stems/demucsstft.*` - 4096 FFT, 1024 hop, periodic Hann, normalized
  forward transform, Nyquist dropped, reflective padding and edge-frame trim;
  CaC layout `[4, 2048, ceil(frames/1024)]`.
- `src/stems/demucsnormalization.*` - unbiased statistics,
  `(x - mean) / (1e-5 + std)` and `x * std + mean`.
- `src/stems/demucsbag.*` - weighted bag averaging of denormalized CaC and
  waveform outputs, with `reset()` for buffer reuse.
- `src/stems/demucslimiter.*` - one shared gain over every stem and the stem
  sum, preserving phase and the mix reconstruction.
- `demucsReconstruct()` - residual + iSTFT, aliasing-safe.
- 37 new tests across the two PRs; full-window round trip 160 dB interior.

### Stems 2 - ONNX Runtime backend (`#19`)

- `STEM_SEPARATION` AUTO/ON/OFF; `cmake/modules/Findonnxruntime.cmake` honors
  `STEM_SEPARATION_ORT_ROOT` and `CMAKE_PREFIX_PATH`; nothing downloads at
  configure time.
- Vendored MIT runtime under `lib/demucs-onnx/`.
- `DemucsOnnxSeparator` and `createDemucsOnnxSeparator()`; bounded worker
  threads (`min(4, max(1, cores/2))`); four-source models only.
- Tiny synthetic ONNX generator `tools/make_test_demucs_onnx.py`; 7 tests.
- `.github/workflows/stems-onnx.yml` downloads ORT on Linux, generates the tiny
  model, builds with `STEM_SEPARATION=ON` and runs the backend tests. It is
  green (37m 53s).

### Stems 3 - native stem export (`#20`)

- Traktor `.stem.mp4` **writing is dropped**; the reader remains.
- `src/stems/stemfileformat.*` - `flac`/`alac`/`aac`,
  `[Stems],FileFormat` and `[Stems],AacBitrate`.
- `src/stems/stemfilewriter.*` - four stereo files plus `stem-set.json`;
  FLAC/ALAC via libsndfile (24-bit), AAC via the existing FDK encoder;
  atomic temp-directory write; refuses an existing set.
- 10 tests: FLAC/ALAC round trips, AAC ADTS smoke test, sidecar, existing-set
  refusal, invalid arguments, temp cleanup.

---

## 5. Next work, in order

### A. Merge status

The implementation and docs stack is merged to `main`. Start at B.

### B. Stems 3 integration (start here)

The writer and format setting exist; they are not wired to the library yet.

1. **Schema revision 41** (coordinate first; `res/schema.xml` is
   collision-prone):
   - Add a `stem_set` table (or columns on the track) for the output directory,
     format, model, sample rate, frame count, AAC bitrate and creation time.
   - Keep `min_compatible="3"` so older builds can still open the library.
   - Add the DAO and migration tests.
2. **Library action and service call**
   - After `DemucsOnnxSeparator::separate()` completes, collect the
     interleaved 8-channel output and call `writeStemSet()` with
     `readStemExportSettings()`.
   - Apply `demucsLimiterGain()` from `#18` before writing: the wrapper
     currently emits raw, unclipped stems.
   - On success, insert the `stem_set` row and emit a progress/status signal.
3. **Settings -> Stems**
   - Bind a combo to `[Stems],FileFormat` (`flac`, `alac`, `aac`) and a
     bitrate control to `[Stems],AacBitrate` (32..512).
   - Use `readStemExportSettings()` / `writeStemExportSettings()`.
4. **Context action**
   - "Save Stems As..." chooses an output folder and a set name; the writer
     refuses an existing name, so generate a unique default.
5. **Playback**
   - Keep the ADR-002 8-channel FLAC cache as the playback path.
   - Direct playback of the exported four-file set is optional and needs a
     `SoundSourceStemSet` that interleaves the four files; do not block the
     export on it.

### C. Stems 4 - background queue and service

- Add `StemService` under `CoreServices`: a persistent low-priority queue for
  single-track and batch jobs, pause/cancel/resume, free-space preflight and
  per-track status (`NotSeparated`, `Queued`, `Separating%`, `Ready`,
  `MissingStemFile`, `Failed`).
- Use `DemucsOnnxSeparator` for the job. It owns the whole-track loop, so the
  service does not need the generic `StemSeparation` window loop.
- On completion, trigger the 8-channel `AnalyzerWaveform` for the cache (not
  for each exported file).
- Files: `src/stems/`, `src/coreservices.*`, `src/analyzer/`.

### D. Backend follow-ups

- Wire the shared limiter and 44.1 kHz resampling into the ONNX wrapper. The
  vendored `lib/demucs-onnx/include/demucs/resample.hpp` is available; the
  input track may be 48/96 kHz.
- Add Windows and macOS ORT CI jobs once runtime distribution is decided.
- Model manager and ADR-005: the **HTDemucs weights are CC BY-NC 4.0**, not
  MIT. A distribution decision is required before shipping weights. The
  `paramix-models` repository is the intended host.

### E. Verification before opening the next PR

```powershell
.\build\mixxx-test.exe --gtest_filter=StemFile*:StemSeparationTest.*:DemucsOnnxSeparatorTest.*
```

Also run `clang-format --dry-run --Werror --style=file` on changed C++ files,
and keep new workflow files separate from `.github/workflows/ci.yml`.

---

## 6. Risks, gotchas and ownership

- **Weights licence:** CC BY-NC 4.0. Do not commit or bundle them. The runtime
  code is MIT.
- **Traktor:** writing is dropped; reading `.stem.mp4` stays. Do not
  reintroduce the NI `moov/udta/stem` writer.
- **ALAC is CAF:** libsndfile writes ALAC in `.caf`, not `.m4a`. AAC is an ADTS
  `.aac` stream, not an M4A container.
- **FDK AAC** is loaded dynamically; the AAC test skips when the library is
  unavailable. The recording feature already has the same dependency.
- **ONNX Runtime** is not in the standard buildenv. The local root is
  `buildenv/onnxruntime-win-x64-1.32.0/onnxruntime-win-x64-1.32.0`; CI
  downloads it.
- **Two STFT implementations:** the standalone `demucsstft` (double precision,
  qm-dsp FFT) and the vendored runtime's `Spectrogram` (float, own FFT). The
  ONNX path uses the latter. Do not mix them in one output without a reason.
- **Shared files:** `CMakeLists.txt`, `res/schema.xml`, `src/track/track.h`,
  `src/engine/engine.h` are collision-prone. `docs/agent-context.md` and
  `.github/workflows/ci.yml` are main-owned; do not edit them.
- **Branches:** `bradley/<topic>`; PR plus checks; no direct pushes to `main`.
- **Docs edits:** the docs PR includes the two pre-existing consolidated
  planning edits plus this handoff. Merge it last.

---

## 7. Copy-paste prompt for a fresh chat

```text
Continue the Paramix stem integration in C:\Users\Bradl\paramix.

Read docs/handoff-stems-integration.md first, then docs/stem-file-export.md,
docs/stems-onnx.md, docs/adr/0001-inference-runtime.md, docs/adr/0002-stem-cache.md
and docs/parallel-work.md.

The implementation stack is merged to `main` (#17-#20); #21 updated the docs.
Start Stems 3 integration at section 5B: schema revision 41, the
Settings -> Stems format controls, the context action, and calling
writeStemSet() from the separation service after applying the shared limiter.
Follow the repository rules in AGENTS.md and docs/parallel-work.md.
```
