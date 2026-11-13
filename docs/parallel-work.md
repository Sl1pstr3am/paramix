# Parallel work: coordination guide for additional contributors

This document tells a second team of agents and developers (starting with Bradley's) what they can
work on **concurrently** with the main Paramix workstream without colliding with it.

Read §1 and §8 before starting anything. Claim a workstream in §3 before writing code.

---

## 1. Ground rules

These are not style preferences; breaking them costs a cancelled CI run or a broken build.

1. **Never sync with upstream Mixxx.** No `git remote add upstream`, no merges or cherry-picks from
   `mixxxdj/mixxx`. The imported tree in this repository is the source of truth. The `upstream`
   remote was deliberately removed.
2. **Do not push to `main` — it is enforced, not just convention.** `main` requires a pull request
   and all five CI jobs (`Repository checks`, `Linux x64`, `Windows x64`, `macOS arm64`,
   `macOS x86_64`). Force pushes and branch deletion are blocked, and unresolved review
   conversations block merging. The workflow uses `concurrency: cancel-in-progress: true`, so a
   push to `main` cancels every build that is already running — including other people's.

   The intended loop for every contributor, including the owner:

   ```bash
   git switch -c bradley/<topic>
   # ... work, commit ...
   git push -u origin bradley/<topic>
   gh pr create --fill --base main
   gh pr merge --squash --auto   # merges once all five checks pass
   ```

3. **Branch naming:** `bradley/<topic>`, for example `bradley/controller-mappings` or
   `bradley/a11y-qml`.
4. **Do not edit these files** without asking the integrator first:
   - `docs/agent-context.md` — the main workstream's handoff log; single owner.
   - `.github/workflows/ci.yml` — CI definitions; single owner. Add *new* workflow files instead.
   - `CMakeLists.txt`, `src/CMakeLists.txt`, `res/schema.xml`, `src/track/track.h`,
     `src/engine/engine.h` — shared structural files where concurrent edits conflict badly.
5. **American English** in all prose: `color`, `behavior`, `license`, `customize`, `analyze`,
   `canceled`, `labeled`, `localized`, `normalize`, `artifact`, `center`, `optimize`, `modeling`,
   `gray`. Prose only — do not rename existing code identifiers.
6. **Use `christianstuart`** in every new link, badge, remote and `gh` command. The old owner name
   redirects but must not be written again.
7. **GPL-2.0 compatibility.** Do not add a dependency without checking. Apache-2.0, GPL-3.0 and
   AGPL-3.0 code cannot be linked into this GPL-2.0-only project. Ask before adding anything.
8. **README.md is deliberately aspirational.** The owner asked for it to describe the target
   feature set as if it already ships. Do not "fix" it by deleting features, and do not treat it as
   a specification. `docs/paramix-stem-beatgrid-ui-plan.md` is the specification.
9. **Real-time audio rules still apply.** The engine callback must not allocate, lock, or block.
   See `AGENTS.md`.

---

## 2. What already exists — do not rebuild it

A frequent waste of effort here is building something the imported tree already has.

| Capability | Status | Evidence |
|---|---|---|
| Per-stem waveform rendering (stacked lanes and overlay) | **Exists** | `src/waveform/renderers/allshader/waveformrendererstem.cpp`, QML wrapper `src/qml/qmlwaveformrenderer.h:638` |
| Per-stem waveform data | **Exists** | `WaveformData::stems[mixxx::kMaxSupportedStems]` in `src/waveform/waveform.h:31`, filled by `src/analyzer/analyzerwaveform.cpp` |
| Per-stem gain/mute/solo/FX, 8-channel deck path | **Exists** | `src/engine/channels/enginedeck.cpp`, `[ChannelN_Stem1..4]` control groups |
| Stem control UI | **Exists** | `res/qml/StemKnob.qml`, `res/qml/Deck/HotcueAndStem.qml`, LateNight stem controls |
| Reading NI `.stem.mp4` (ALAC and AAC) | **Exists** | `src/sources/soundsourcestem.cpp`, `src/track/steminfoimporter.cpp`, fixtures in `src/test/stems/` |
| Downbeat / bar / meter in the beatgrid | **Absent** | `src/track/beats.h` has markers plus one tempo; no downbeat field |
| Stem **separation** (any model) | **Absent** | no inference runtime anywhere in the tree |
| `.stem.mp4` **writing/export** | **Absent** | no exporter; the path is read-only |

---

## 3. Claimable workstreams & consolidated task backlog

Workstreams W1 (#3), W2 (#4), W4 (#6), W5 (#5), W6 (#15), and the `rbxport` beatgrid engine (#16) are **merged on `main`**.

The remaining project backlog on the [Program Board](https://github.com/users/christianstuart/projects/2) is consolidated into **8 independent tasks** so multiple agents can work in parallel without file collisions:

**Current state (2026-11-13):** Stems 1 and 2 are in review (#17, #18, #19); the native stem export is in review (#20). All five CI jobs are green on each. Stems 4 (the background queue) is the next implementation task. The program board carries the same status.

### 1. Beatgrid — Rekordbox-style phrase labeling & visualization option
- **Scope:** Extend `src/proto/beats.proto` additively (`BarInfo` + phrase segments under existing version strings; fix `Beats::operator==`), preserve `GridPhase::phraseStarts` in `analyzerrbxportbeats.cpp` and classify segments into Rekordbox-style phrase labels (`Intro`, `Up`, `Down`, `Verse`, `Bridge`, `Chorus`, `Outro`), import Rekordbox `PSSI` (`SECTION_TAGS_SONG_STRUCTURE`) in `rekordboxfeature.cpp`, and add a user preference to toggle between **Standard Bar** lines and **Rekordbox Phrase Style** (colored, labeled phrase blocks/strip).
- **Files:** `src/proto/beats.proto`, `src/track/beats.*`, `src/analyzer/plugins/rbxport/*`, `src/library/rekordbox/rekordboxfeature.cpp`, `src/waveform/renderers/*waveformrenderbeat*`.

### 2. Stems 1 - Host STFT/iSTFT & CaC-split DSP pipeline (implemented, PRs #17/#18)
- **Status:** In review. #17 adds the host transform; #18 adds normalization, bag averaging, the shared limiter and `demucsReconstruct`. 52 tests pass locally and all five CI jobs are green.
- **Scope:** Cut HTDemucs v4 at the complex-as-channels boundary: a host C++ STFT (`n_fft=4096`, `hop=1024`, Hann window, Nyquist + two-frame edge trim) producing `mix [B, 2, 343980]` and `mag [B, 4, 2048, 336]`, `stem[s] = wav[s] + ISTFT(spec[s])`, denormalized bag blending before one inverse transform, and a shared scalar peak limiter.
- **Files:** `src/stems/demucsstft.*`, `src/stems/demucsnormalization.*`, `src/stems/demucsbag.*`, `src/stems/demucslimiter.*`, `src/test/`.
- **Remaining:** the ONNX wrapper still emits raw stems (the limiter is not wired into it) and 44.1 kHz resampling is not wired.

### 3. Stems 2 - ONNX Runtime inference backend (implemented, PR #19)
- **Status:** In review. The MIT header-only demucs-onnx runtime is vendored under `lib/demucs-onnx/`; `DemucsOnnxSeparator` wraps `demucs::Separator`; `STEM_SEPARATION` is AUTO/ON/OFF; a dedicated Linux ORT CI workflow runs.
- **Scope:** ONNX Runtime session and execution-provider fallback, chunking, track-level normalization, overlap-add, four-stem output and the factory seam. Validated with the demucs-onnx `htdemucs` graph on Windows x64 CPU.
- **Licensing:** the runtime is MIT; the HTDemucs weights are CC BY-NC 4.0, not MIT, and are not committed.
- **Files:** `lib/demucs-onnx/`, `src/stems/demucsonnxseparator.*`, `src/stems/stemseparatorfactory.*`, `cmake/modules/Findonnxruntime.cmake`, `.github/workflows/stems-onnx.yml`, `docs/stems-onnx.md`.
- **Remaining:** wire the shared limiter and resampling, add Windows/macOS ORT CI, and the model manager/ADR-005.

### 4. Stems 3 - Native stem set export (FLAC/ALAC or AAC) (implemented, PR #20)
- **Status:** In review. Traktor `.stem.mp4` writing is dropped; the reader remains. A set is four stereo files plus `stem-set.json`.
- **Scope:** FLAC (default, libsndfile 24-bit), ALAC (libsndfile CAF/ALAC 24-bit) or AAC (FDK AAC ADTS, default 256 kbps). `[Stems],FileFormat` and `[Stems],AacBitrate`; atomic temp-directory writes; existing-set refusal; sidecar metadata.
- **Files:** `src/stems/stemfileformat.*`, `src/stems/stemfilewriter.*`, `src/test/stemfilewritertest.cpp`, `docs/stem-file-export.md`.
- **Remaining:** schema rev 41 link, Settings/UI, service integration and direct playback of exported sets.

### 5. Stems 4 — Background stem separation queue & worker service (`StemService`)
- **Scope:** Persistent low-priority job queue owned by `CoreServices` supporting single-track and batch separation, pause/cancel/resume, free-space preflight, per-track progress signals (`NotSeparated`, `Queued`, `Separating%`, `Ready`, `MissingStemFile`, `Failed`), and automatic 8-channel `AnalyzerWaveform` trigger upon completion.
- **Files:** `src/stems/`, `src/coreservices.*`, `src/analyzer/`.

### 6. Waveform — 4-stem tinted composite mode, labels & 60 fps batching
- **Scope:** Add a selectable **tinted composite** mode alongside stacked lanes in `allshader::WaveformRendererStem` and its QML wrapper, keep muted stems visible at dimmed opacity, add per-stem labels with the color-blind-safe palette, and fix the `kPixelPerStrip = 2` scene-graph batching to hold ≤ 8 ms CPU/frame at 60 fps (4 decks @ 1080p).
- **Files:** `src/waveform/renderers/allshader/waveformrendererstem.*`, `src/qml/qmlwaveformrenderer.*`, `res/shaders/`.

### 7. UI Overhaul — Paramix QML skin, design system & stems/phrase controls
- **Scope:** Build `res/qml/Theme` design tokens and the default `res/skins/Paramix/` QML skin (2/4-deck variants, stacked/tinted waveform toggle, Standard Bar vs. Rekordbox Phrase toggle, 4-row deck stem mixer strip with Acapella/Instrumental complement buttons, Library stem status column + context menu, and `Settings → Stems` page).
- **Files:** `res/skins/Paramix/`, `res/qml/`, `src/qml/`, `src/preferences/dialog/`.

### 8. W3 — Packaging, Paramix rebrand & `~/.paramix` migration wizard
- **Scope:** Finish cross-platform packaging (Windows x64 installer, macOS universal bundle with `-DQML=ON`, Linux AppImage/Flatpak/deb), `Paramix 0.1 (based on Mixxx 2.7.0)` identity/icons/bundle IDs, `NOTICE` + `docs/third-party-models.md`, first-run `~/.mixxx` → `~/.paramix` migration wizard, and register W4's fixture tests in `CMakeLists.txt`.
- **Files:** `packaging/**`, `res/linux/**`, `res/images/**`, `docs/third-party-models.md`, `CMakeLists.txt`.

---

## 4. Ownership map

Update the Owner column in your first PR. `main` means the primary workstream; do not edit those
paths.

| Area | Paths | Owner | Notes |
|---|---|---|---|
| Handoff log | `docs/agent-context.md` | main | Single owner |
| Program plan | `docs/paramix-stem-beatgrid-ui-plan.md` | main | Ask before editing |
| CI definitions | `.github/workflows/ci.yml` | main | Add new files instead |
| Beatgrid / downbeats | `src/track/beats*`, `src/analyzer/**` | main | Planned work, collision-prone |
| Stem separation | `src/engine/**`, new separator code | main | Planned work |
| Stem export | `src/stems/`, `src/sources/**`, `src/track/steminfo*` | main | Planned work |
| Paramix QML skin | `res/qml/**` (new skin) | main | Coordinate before editing shared QML |
| Controller mappings | `res/controllers/**` | **free** | W1 |
| QML accessibility/localization | `res/qml/**` (existing files) | **free** | W2 — announce first |
| Packaging | `packaging/**`, `res/linux/**` | **free** | W3 |
| Tests and fixtures | `src/test/**`, `tools/**` (new) | **free** | W4 |
| Model hosting tools | `tools/**` (new), `docs/**` (new) | **free** | W5 |
| New workflows | `.github/workflows/*.yml` (new) | **bradley** | W6 — QML minimum-Qt guard |

---

## 5. Verification before opening a pull request

```bash
# Repository checks: conflict markers, whitespace, YAML/JSON, clang-format
./tools/paramix_verify.sh

# Local formatting fix for changed C++ files
clang-format -i path/to/changed.cpp

# Pre-commit hooks (some need Python 3.10+ and will not run locally on 3.9)
pre-commit run --files <changed files>
```

CI runs five jobs: `checks`, `Linux x64`, `Windows x64`, `macOS arm64`, `macOS x86_64`.

- **The clang-format gate checks changed C++ files only.** The imported tree is not wholly
  formatted, so a whole-tree check cannot pass; touching a legacy file means formatting that file.
- **Linux runs 1 302 tests; 38 are upstream `DISABLED_` cases** — that is expected, not a failure.
- **macOS jobs build but do not run tests**, matching upstream. Green there means "compiles".
- **No `xvfb` is needed:** `src/test/main.cpp` sets `QT_QPA_PLATFORM=offscreen` itself.
- A full clean build takes ~20–40 minutes; a push that touches no file under `src/**`,
  `CMakeLists.txt` or `cmake/**` hits the ccache and finishes in minutes. Keep documentation-only
  changes in their own PR.

---

## 6. Traps worth knowing

1. **The 3.7 GB build environment can truncate.** `file(DOWNLOAD)` in `CMakeLists.txt` has no
   `STATUS` check, so a cut-off transfer appears as `SHA256 checksum mismatch`. The workflow now
   pre-fetches with `curl --retry` and verifies; if you see that error locally, delete
   `<BUILDENV_BASEPATH>/<name>.zip` and retry.
2. **Do not create a local commit without setting an identity.** This repository has no global git
   identity on the owner's machine.
3. **`docs/old-context.md` is a verbatim historical transcript.** It keeps British spelling and the
   pre-rename username. Do not edit it.
4. **Do not commit build outputs, `.DS_Store`, or model files.** Models live in `paramix-models`.
5. **`README.rtf`-style artifacts:** if a Markdown file is ever saved as RTF by an editor, the
   repository check will not catch it. Verify with `file README.md` if a doc suddenly looks wrong.
6. **macOS x86_64 builds with `-DQML=OFF`**, so a QML-only change is not verified on that target.
   Do not assume a green x86_64 job means your QML loads.

---

## 7. Coordination protocol

1. Read this document and §2 before choosing work.
2. Claim a workstream by editing §4 in a small pull request, or open an issue titled
   `workstream: <name>`.
3. Announce anything that touches a **shared** path in §4 before you start, even if the Owner column
   says "free".
4. Keep pull requests small and scoped to one workstream. A PR that mixes packaging, tests and QML
   will conflict with everything.
5. If CI fails for a reason unrelated to your change (runner download, ccache restore), re-run the
   job before investigating your diff.

---

## 8. Scope discipline

Two things are deliberately **not** on this list because they are the main workstream's core
deliverables: **stem separation** and **downbeat-aware beatgrids**. Both require design decisions
that are still open (inference runtime, model choice, grid format) and touch the most
collision-prone files in the tree. If you want to contribute there, start with the ADR or the spike
rather than the implementation.
