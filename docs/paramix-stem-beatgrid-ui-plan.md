# Paramix — Beatgrid, Stem Separation & UI Program Plan

**Status:** Proposal v0.2 (decisions locked) · 2026-11-11
**Repository:** `paramix` (working tree is a Mixxx 2.7.0-alpha fork; remote `github.com/christianstuart/paramix`)
**Scope of this document:** planning and architecture only. No source code was modified to produce it.

---

## 0. Executive summary

The brief has four parts: (1) change the beatgrid engine, (2) add commercial-grade 4-stem separation like Traktor, (3) show individual 4-stem waveforms, (4) generally change the UI.

Three findings shape the plan:

1. **Most of the playback half already exists.** This fork ships a complete NI `.stem.mp4` playback path: 8-channel deck rendering, four per-stem control groups (`[ChannelN_Stem1..4]`), per-stem QuickFX, per-stem waveform peaks and a working 4-stem renderer. The gap is **generation** (separating arbitrary tracks), **persistence** (stem cache + metadata), and **polish** (lanes, status, model management, UI).
2. **The beatgrid data model is already tempo-map-capable.** `mixxx::Beats` stores markers plus a terminal tempo, so variable tempo is representable today. What is missing is **detection quality** (only Queen Mary + SoundTouch heuristics), **downbeats/bars**, **confidence**, **hypotheses**, and **non-destructive editing**.
3. **Licensing decides the ML stack.** Mixxx is GPL-2.0. HTDemucs (code *and* weights MIT) and Beat This! (code *and* weights MIT, with an ONNX export) are the license-safe, deployable choices. madmom/Essentia weights are CC BY-NC-SA; Essentia is AGPLv3; aubio is GPLv3; BS-RoFormer and MDX23C community weights ship with no license — those can be user-supplied options, not bundled defaults.

**Recommendation:** execute **Option B ("Traktor-class")** in ~20–24 weeks with 6–8 concurrent agents and a hard human validation gate, preceded by a one-week repository baseline. It delivers: a bar- and downbeat-aware grid (ADR-003: downbeat phase, confidence, tempo maps, non-destructive editor), HTDemucs-based offline separation with a model manager and cache, a stem-native waveform and mixer UI, and a QML design system. Option A is the first two milestones of B and can ship in ~10–12 weeks if the schedule is tight. Option C (real-time separation, full QML rewrite) should be deferred: live separation is 0.5–2 s latent at degraded quality even on GPU, and Traktor itself pre-computes stems.

### Decisions locked (2026-11-11)

| # | Decision | Implementation consequence |
|---|---|---|
| 1 | **Default separator:** `htdemucs_ft` (4-model bag, ~630 MB fp16) | Best bundled quality; ~2 min per 4-min track on M-series CPU, seconds with CoreML/DirectML; `htdemucs` single remains the low-end fallback path. |
| 2 | **Trigger:** on-demand, with optional **batch** separation | Job queue accepts one or many tracks; "Separate" and "Separate selected/all" actions; no auto-separate on import by default; pause/cancel/resume. |
| 3 | **Downbeats/bars:** required in **v1** | ADR-003 bar info (downbeat phase, bar lines) and "set downbeat" editing are v1 acceptance criteria, not deferred; bar-aware quantise/sync land as flags in the same release. |
| 4 | **Traktor interoperability:** required | `.stem.mp4` **export** (4 ALAC/AAC streams + NI JSON atom) is v1 scope; NI `.stem.mp4` import stays compatible; validate round-trip and Traktor loading. |
| 5 | **UI:** new QML skin **default** and **highly customizable** | Paramix QML skin ships as the default; design tokens, layout variants (2/4 deck, stem lane density), color schemes, and user-overridable QML; legacy skins remain selectable. |
| 6 | **Platforms:** Windows x64, Linux x64 (+ arm64 where feasible), macOS arm64 **and** x86_64 | Universal macOS build; ONNX Runtime EPs mapped per platform; no iOS/Android in v1. |
| 7 | **Model hosting:** **GitHub Releases**, **download-only** (see §5.9) | No model files in git or installers; manifest + SHA-256 + MIT license shown before download; first separation downloads `htdemucs_ft` (~630 MB fp16) with progress/resume; an offline bundle remains a packaging add-on, not an architecture change. |
| 8 | **Rebrand:** **Paramix** | Product name, icons, bundle/package IDs, desktop entry, skin name, docs, website strings; GPL notices and upstream attribution retained. |
| 9 | **Model release location:** dedicated **`paramix-models`** repository | Model assets and their manifests live outside the app repo, so app clones and Releases stay small; the manifest URL is the only coupling. |
| 10 | **Data directory:** **migrate to `~/.paramix`** | First-run migration wizard copies the library DB, settings and controller mappings; a downgrade path keeps the old directory intact until the user confirms. Supersedes the earlier brand-only recommendation in §6.9. |
| 11 | **Versioning:** start at **`0.1`**, milestone releases | `0.1`, `0.2`, … per shipped milestone; About shows "Paramix 0.1 (based on Mixxx 2.7.0)". |
| 12 | **GPU policy:** **auto with opt-out** | CoreML (macOS), DirectML/CUDA (Windows), CUDA (Linux), CPU fallback; Settings can force CPU. |
| 13 | **`.stem.mp4` codec:** **user-selectable (ALAC or AAC)** | Settings chooses the export/analysis codec; the export dialog shows the size/time trade-off. No silent default. |

---

## 1. The brief, decomposed into acceptance criteria

The **Status today** column is the important one: it records what the imported tree already
provides, so phases below plan integration and polish rather than rebuilding working code.

| # | Outcome | Measurable acceptance | Status today (2026-11-11) |
|---|---|---|---|
| O1 | **Beatgrid engine** | Downbeat-aware grids; accuracy beats Queen Mary on a labeled fixture set (beat F ≥ 0.90 electronic / ≥ 0.85 mixed repertoire, downbeat F ≥ 0.75); variable-tempo detection for non-4/4 material; non-destructive edits; old libraries open unchanged. | **Not started.** `Beats` stores 0..N markers + one tempo marker and has no downbeat, bar, meter or confidence field; detection is Queen Mary or SoundTouch, no ML. This is genuine new work. |
| O2 | **Stem separation** | 4 stems (drums/bass/vocals/other) from any supported audio file, fully offline after the one-time model download; default **`htdemucs_ft`** quality (MUSDB SDR ≈ 9.2 dB avg; vocals ≥ 8.0 dB); on-demand and batch jobs with progress/cancel/resume; cached so the second load is instant; per-stem gain/mute/solo/FX; instant acapella/instrumental buttons; **required** `.stem.mp4` export for Traktor; ~2 min per 4-min track on M-series CPU, seconds with CoreML/DirectML. | **Not started for generation.** Playback, per-stem mixing and FX of *existing* `.stem.mp4` files already work (§3.2), so the new work is inference, cache, model manager, batch jobs, library status and export — not the mixer. |
| O3 | **4-stem waveform** | Each stem visible as its own labeled, colored lane **and** as a tinted composite (both modes), reacting to mute/solo/gain; muted stems stay visible but dimmed; downbeat accents visible; 60 fps with 4 decks at 1080p; no audio-thread stalls. | **Largely present.** `allshader::WaveformRendererStem` already draws per-stem stacked lanes or an overlay, with stem colors and live mute/gain, and `WaveformData::stems[4]` is already populated by `AnalyzerWaveform` from an 8-channel source (§3.3). Remaining: tinted **composite** mode, downbeat accents, and performance validation at 4 decks/1080p. |
| O4 | **UI overhaul** | A coherent, rebranded **Paramix** QML skin as the default (deck, stem mixer, library badges, settings), highly customizable via design tokens/layout variants/color schemes; color-blind-safe stem palette + labels; keyboard/controller accessible; localized; responsive at 1280×720 and up on Windows, Linux, macOS arm64 and macOS x86_64. | **Not started.** Only `LateNightQML` exists; library and preferences are still Qt Widgets (§3.3). The stem *controls* it will host already exist. |

**What "commercial-grade" means here** (from Traktor/Serato/rekordbox behavior): offline and private; separation runs in the background without touching the audio thread (Traktor Pro 4 uses iZotope RX offline, ~30–60 s per track, right-click per track); results are cached and reusable (Serato pre-computes a "Stems crate" in ~20 s per track and stores roughly 5× an MP3 per track); stems are individually mixable with gain/mute/solo and per-stem FX (rekordbox offers per-stem gain and FX; Traktor keeps FX deck-level); instant "acapella/instrumental" complement buttons; waveforms show the stems legibly in both stacked and tinted modes; the feature survives restarts, library moves, model updates and old projects; no cloud dependency; predictable CPU/storage budgets; and playback stays at 10–20 ms latency with 128–256 frame buffers.

---

## 2. Constraints and guardrails

| Constraint | Consequence |
|---|---|
| **GPL-2.0** | Every linked library must be GPLv2-compatible. Apache-2.0 (TensorFlow/TFLite) is not GPLv2-compatible; AGPLv3 (Essentia) and GPLv3 (aubio) cannot be combined with GPLv2-only code. Model weights need their own license audit. |
| **Real-time audio thread** | Separation, disk I/O, model loading and allocation happen off the engine thread. `EngineDeck::processStem()` already runs in the RT callback — no new locks/allocations there. |
| **Cross-platform** | macOS (arm64/x86_64), Windows (x64), Linux (x64/arm64). Inference via ONNX Runtime execution providers: CoreML (macOS), DirectML/CUDA (Windows), CUDA/CPU (Linux), CPU always. |
| **Offline-first** | No cloud separation. Model files are downloaded once with checksums and an explicit license, or pre-installed by a packager. |
| **Backward compatibility** | Existing `.stem.mp4` files must keep working; `library.beats` blobs must remain readable; a downgrade must not silently destroy grids, so new grid fields are additive under the existing version strings (ADR-003). |
| **Qt 6.4+ / QML** | QML builds require Qt ≥ 6.4 (`ComponentBehavior: Bound`); the QML skin is the modern UI path. Qt Widgets remain for library/preferences initially. |
| **Human validation** | Audio quality is subjective; every milestone ends with a real DJ test on hardware (controller + speakers), not just CI. |
| **Repo baseline** | The import is uncommitted, `.clang-format`, `.pre-commit-config.yaml`, `.github/` and `.gitignore` are missing. Multi-agent work needs a green, formatted, CI-backed baseline first. |

---

## 3. Current state (evidence from the tree)

### 3.1 Beatgrid

> Full audit with citations, the ordered work list and the top risks:
> [`docs/beatgrid-audit.md`](beatgrid-audit.md). The summary below predates it and stays accurate,
> but the audit adds the serialization hazards.

- **Model:** `mixxx::Beats` (`src/track/beats.h`) = 0..N `BeatMarker{position, beatsTillNextMarker}` + exactly one terminal tempo marker. Constant tempo = no markers. Serialized as `BeatGrid-2.0` (constant) or `BeatMap-1.0` (markers); `BeatGrid-1.0` still readable. Positions are int32 frames on the wire, double in memory.
- **Detection:** `AnalyzerBeats` (`src/analyzer/analyzerbeats.cpp`) selects one of two hard-coded plugins: **Queen Mary** (`lib/qm-dsp`, `src/analyzer/plugins/analyzerqueenmarybeats.cpp`) and legacy **SoundTouch** (BPM-only). The plugin is resolved by string id through an `if/else`; there is no dynamic loading. `BeatUtils` (`src/track/beatutils.cpp`) then irons the beat list (25 ms phase tolerance, max 1 outlier, ≥16 beats per constant region), picks the longest constant region, and snaps BPM with a fixed ladder (integer → 0.5 below 85 → 2/3 above 127 → 1/3 → 1/12). Half/double ambiguity is resolved heuristically, not by evidence.
- **No downbeats/bars/meter/confidence/hypotheses anywhere.** The proto (`src/proto/beats.proto`) declares `Beat.source`, `Beat.enabled` and `Bpm.source` but never writes/reads them — free schema room.
- **Persistence:** `library.beats` BLOB + `beats_version` / `beats_sub_version` / `bpm` / `bpm_lock` (schema revision 40, `res/schema.xml`; DAO in `src/library/dao/trackdao.cpp`). Unknown versions are dropped and replaced by a frame-0 grid from cached BPM (`trackdao.cpp`), so downgrades lose phase.
- **Consumers:** `BpmControl` (sync/phase), `QuantizeControl`, `LoopingControl` (beatloop, beatjump, loop move), `EngineSync`. All are beat-only; no bar-aware behavior.
- **Editing:** `DlgTrackInfo` BPM tab and `WTrackMenu`; `Beats::trySetBpm` **collapses tempo maps**, `tryScale` can fail on non-divisible segments; undo stack is 10 deep. There is no downbeat editing.
- **Tests:** good unit coverage of `Beats` (serialization, iterators, BPM), Serato import, loop/sync; **no end-to-end detection test, no audio fixture with a golden grid, no benchmark**. Known defects: `operator==` ignores sample rate; `numBeatsInRange` is O(n²); `getBpmInRange` allocates on every grid edit.

### 3.2 Stems

- **Playback works:** `SoundSourceSTEM` (`src/sources/soundsourcestem.cpp`) reads the NI 5-stream layout (pre-mix + 4 stereo stems), exposes 8 channels (`ChannelCount::stem()`), and `EngineDeck::processStem()` (`src/engine/channels/enginedeck.cpp`) renders 4 stems separately, applies per-stem QuickFX, gain/mute smoothing, VU, then mixes to stereo. `kMaxSupportedStems = 4` (`src/engine/engine.h`).
- **Controls/UI:** per-deck groups `[ChannelN_Stem1..4]` with `volume`, `mute`, `color`, VU; `WTrackStemMenu` loads a specific stem or the premix; `LateNightQML/Waveforms/StemControls.qml` and the Deere/Tango/LateNight `stem_channel.xml` skins already exist; Traktor S4 MK3 controller screens include stem views.
- **Waveform data:** `WaveformData::stems[4]` (one byte per stem per frame, computed as max(L,R)), `Waveform-6.1` under `__STEM__`, `AnalyzerWaveform` stores per-stem peaks for 8-channel sources, and `allshader::WaveformRendererStem` renders overlay or stacked lanes with stem colors and live mute/gain.
- **Missing:** separation/generation, stem cache, model management, export, persistence of `StemInfo` (labels/colors are re-read from the `.stem.mp4` JSON atom every time), and status/progress in the library. Stem detection is by **file extension**; generated stems have no `.stem.mp4` file, so the source path needs a hook.

### 3.3 Waveform & UI

- **Rendering:** `allshader::` renderers are `rendergraph` geometry nodes with two backends: QtQuick scene graph (QML skins) and QOpenGL (widget skins). Shaders live in `res/shaders/`. Frames are driven by `VSyncThread` (default 60 fps); each frame rebuilds vertex data into a reused `Geometry`.
- **Stem renderer today:** overlay (outline + fill per stem) or stacked (`breadth/4` lanes); 6 vertices × 4 stems × 2 layers = **48 vertices per pixel column**, CPU-built per frame. Under the scene-graph backend `kPixelPerStrip = 2`, halving horizontal precision — already a quality compromise.
- **Stem renderer API (verified 2026-11-11):** `allshader::WaveformRendererStem`
  (`src/waveform/renderers/allshader/waveformrendererstem.{h,cpp}`, 289 lines) derives from
  `WaveformRendererSignalBase` **and** `rendergraph::GeometryNode`; it holds
  `m_pStemGain`/`m_pStemMute` `ControlProxy` vectors, `m_stackOrder` (`QVarLengthArray<int, 4>`),
  and exposes `setSplitStemTracks`, `setReorderOnChange`, `setOutlineOpacity`, `setOpacity`. It is
  wrapped for QML as `QML_NAMED_ELEMENT(WaveformRendererStem)`
  (`src/qml/qmlwaveformrenderer.h:638`, wired in `.cpp:313-349`). Slip mode already draws
  per-stem boxes (`waveformrendererslipmode.cpp:96-120`). `res/qml/StemKnob.qml` and
  `res/qml/Deck/HotcueAndStem.qml` provide the control surface.
  **Consequence:** four labeled lanes, per-stem color and mute/gain-reactive display are existing
  behavior. What O3 still needs is a tinted **composite** mode, downbeat accents, and a
  performance budget check — not a new renderer.
- **Skin system:** legacy XML skins (Deere, LateNight, Tango, Shade) parsed by `legacyskinparser.cpp`; one QML skin (`LateNightQML`) with `skin.ini` + `main.qml` + `MainWindow.qml` (973 lines). **The library and preferences are still Qt Widgets** (`Library.qml` embeds `Mixxx.LegacyLibraryItem`), and preferences are being ported piecemeal (`res/qml/Settings/*`).
- **Accessibility/localization debt:** effectively no `Accessible.*`/`accessibleName`; the QML skin has 0 `qsTr()` calls (hardcoded English) while the widget UI is fully translated.

### 3.4 Baseline status (was: gaps that block multi-agent work)

All four items below were resolved in Phase −1; see `docs/baseline-report.md` for measurements.

1. ~~The Mixxx import is untracked~~ — committed; `main` is at a green, CI-verified commit.
2. ~~`.clang-format`, `.pre-commit-config.yaml`, `.gitignore`, `.clang-tidy`, `.github/` missing~~ —
   restored, repaired and verified idempotent.
3. ~~No CI~~ — five jobs (checks, Linux x64, Windows x64, macOS arm64, macOS x86_64) green on one
   commit, with a clang-format gate on changed C++ files.
4. `AGENTS.md` policy for this fork allows AI-assisted development and does not require AI
   disclaimers; the plan still keeps human review for audio quality.

Remaining baseline-adjacent gaps (not blockers): macOS jobs build but do not run tests, matching
upstream; macOS x86_64 builds with `-DQML=OFF`; no local Qt toolchain on the owner's workstation.

---

## 4. Program options (choose one)

### Option A — "Momentum": focused upgrade (≈10–12 weeks, 3–4 agents)

Keep the current grid representation and engine. Add an ONNX beat tracker behind the existing analyzer plugin interface, expose confidence and downbeat hints in the sub-version string, and improve half/double handling. Add offline HTDemucs separation with a downloaded model, an 8-channel FLAC cache, and wire generated stems into the existing 8-channel path. Improve the stem waveform (lanes, labels, colors) and refresh the QML skin. No schema migration beyond a stem-cache table.

**Why choose it:** fastest path to a usable "stems for any track" feature; smallest regression surface; can be built by a small team. **What you give up:** no first-class bars/downbeats, destructive grid editing remains, one separation model, modest visual change.

### Option B — "Traktor-class": the recommended program (≈20–24 weeks, 6–8 agents)

Everything in A, plus: an **additive bar/downbeat extension** to the existing grid format (ADR-003) carrying downbeat phase, confidence, source and alternative hypotheses without changing the version strings; a non-destructive editor and bar-aware behaviors (downbeat lines, optional bar quantise/sync, bar loops); **quality tiers** for separation (HTDemucs default, optional user-supplied RoFormer-class "Max" tier) with a proper model manager (download, checksum, license, GPU/CPU selection); a persistent job queue with resume and GC; a stem-native **4-lane waveform** with per-stem labels/colors/solo state and downbeat accents; a **new QML skin and design system** with a stem mixer strip and separation status; `.stem.mp4` export for Traktor interoperability.

**Why choose it:** it matches the brief end-to-end and leaves a maintainable architecture. **Cost/risk:** schema work, ML packaging across three OSes, UI scope, and a longer validation cycle.

### Option C — "Platform leap" (≈9–12 months, 10+ agents)

B plus real-time/streaming separation, warp-marker/tempo-curve grids ("Ableton-style"), a full QML rewrite of the library and preferences, and mobile targets. **Why defer:** live separation is not commercially viable yet (0.5–2 s latency at degraded quality; CPU cannot sustain 1 s chunks; even Traktor pre-computes stems), and the full QML rewrite multiplies QA surface before the stem UX is proven.

### Comparison

| | A Momentum | **B Traktor-class (recommended)** | C Platform leap |
|---|---|---|---|
| Duration | 10–12 wks | **20–24 wks** | 9–12 months |
| Concurrent agents | 3–4 | **6–8** | 10+ |
| Downbeats/bars | hint only | **first-class** | first-class + warp |
| Separation | HTDemucs only | **tiers + model manager** | real-time + tiers |
| Waveform | polish | **4-lane redesign** | per-stem stereo + zoom |
| UI | skin refresh | **new QML skin/design system** | full QML rewrite |
| Schema change | cache table | **bar info + cache** | + warp tables |
| Risk | low | **medium** | high |
| Ship value | good | **highest** | speculative |

**Recommendation:** fund **B**, structure it so **A is milestone 1** (shippable), and keep C's real-time separation as a research spike behind a feature flag, not a commitment.

---

## 5. Area options with descriptive summaries

### 5.1 Beat detection

**Option 1 — Tune Queen Mary (S).** Keep qm-dsp, add better phase/half-double heuristics and confidence from the detection function. No new dependencies, no model download, no license work. Accuracy improves modestly; downbeats remain unavailable; still fails on rubato/live material.

**Option 2 — Beat This! via ONNX Runtime (recommended, S/M).** Beat This! (CPJKU/ISMIR 2024) is the only SOTA tracker found with **MIT code and MIT weights**, an existing ONNX export (82 MB fp32; log-mel input, 22.05 kHz mono, 1024-point FFT, hop 441) that outputs **beat and downbeat logits**, and no DBN post-processing, so it tolerates tempo changes. It runs in `AnalyzerBeats` behind the existing `AnalyzerBeatsPlugin` interface; Queen Mary stays as fallback on unsupported platforms and for fast analysis.

**Option 3 — All-In-One / BeatNet (S/M, conditional).** All-In-One also emits downbeats/meter/structure and is small, but its weights need a license check; BeatNet is strong but its shipping weights also need verification. Use only if Beat This!'s downbeat quality is insufficient.

**Option 4 — madmom / Essentia / aubio (rejected).** madmom's pretrained models are CC BY-NC-SA (non-commercial); Essentia is AGPLv3; aubio is GPLv3. All conflict with GPL-2.0 distribution. They can be used offline for evaluation only, never linked or shipped.

**Recommendation:** Option 2, with Option 1 retained as the always-available fallback and Option 3 as a downbeat quality upgrade. Evaluate on a labeled fixture set before replacing the default.

### 5.2 Grid representation

**Option 1 — Rich detection, same wire format (S).** Keep `BeatGrid-2.0`/`BeatMap-1.0`; encode confidence/downbeat phase in `beats_sub_version` and store the winning hypothesis only. Zero migration risk, but bars are not real semantics and hypotheses are recomputed. Rejected: bars need to be first-class data, not a string convention.

**Option 2 — Additive bar info under the existing version strings (recommended, S–M).** Add an optional `BarInfo` message (`beats_per_bar`, zero-based `first_beat_in_bar`) to `BeatGrid` and `BeatMap`, leaving the version strings unchanged; see [`docs/adr/0003-grid-format.md`](adr/0003-grid-format.md). Absence of `bar` means unknown, never 4/4. Older builds ignore the unknown protobuf field and keep a valid grid, which avoids the destructive frame-0 placeholder that an unrecognized version string triggers. No schema revision and no dual-write. Confidence and detector id stay in `beats_sub_version` until a later decision.

**Option 3 — Layered model with separate hypothesis tables (L).** New `BeatGrid` value type plus `beatgrid_hypotheses` table, adapters to the old API, full editor. Cleanest long-term, highest migration/regression risk; only worth it if warp/tempo curves (Option C) are committed.

**Recommendation:** Option 2, as specified in ADR-003. It makes downbeats real data without a migration, and leaves Option 3 available as a later, mechanical step if warp/tempo curves are committed.

### 5.3 Separation models

| Model | Code / weights license | Quality (MUSDB SDR) | Speed (4-min track) | Deployability | Verdict |
|---|---|---|---|---|---|
| **HTDemucs v4 / htdemucs_ft** | **MIT / MIT** | 9.16 avg (vocals 8.24, drums 10.88, bass 11.76, other 5.74); ft 9.2 | ~1–2 s on RTX 5090; ~2 min (ft bag) / ~40 s (single) on M4 Pro CPU; ~4 min on 4 CPU threads via demucs.cpp | ONNX export practical (demucs-onnx, MIT, <1e-3 parity), ORT CoreML/DirectML/CUDA/CPU; 166 MB fp16 / 316 MB fp32 | **Default** |
| **BS-RoFormer / Mel-Band RoFormer** | MIT code; community weights **no license** | 9.65 avg (vocals 11.08) — best | ~1.5–2 s GPU; slow CPU | ONNX/TensorRT export exists; 0.6–1.2 GB | **Optional user-supplied "Max" tier**, never bundled |
| **MDX23C (TFC-TDF v3)** | MIT code; UVR weights **no license** | 7.15 avg; vocals 10.17 | Fast (~209× RTX) | Ships as ONNX in UVR; ORT-native | Optional fast/vocals tier only |
| **Open-Unmix** | MIT code; `umxhq` MIT, `umxl` **CC BY-NC-SA** | ~6.3–6.9 | Fast, small (~35 MB/stem) | umx.cpp exists | Baseline; only `umxhq` eligible |
| **Spleeter** | MIT code/weights | ~5 (not commercial-grade) | Very fast | TensorFlow/TFLite — **Apache-2.0, GPLv2 conflict** | **Reject** |
| **UVR community models** | App MIT; models mostly unlicensed | Varies | Varies | ORT | Use as UX reference only |

**Speed anchors:** RTX 5090 RTFx: htdemucs 200×, MDX23C 209×, BS-RoFormer 109×, Mel-Band 170–194×. Apple M4 Pro CPU: htdemucs_ft bag RTF 0.49, single specialist 0.20; MPS 0.07. demucs.cpp CPU-only: ~4 min per 4-min track with 4 threads. Precedent: GPL-3.0 **xLights** ships HTDemucs in C++ via ONNX Runtime with CoreML/DirectML/CPU (`src-core/media/StemSeparator.cpp`) — a directly portable pattern.

**Recommendation:** HTDemucs v4 as the bundled default (single model, fp16, CoreML/DirectML when available), with an optional "Max quality" tier that downloads only after showing the model license and, for unlicensed community weights, only accepts user-supplied files. Do not bundle RoFormer/MDX23C/UVR weights. Evaluate quality against MUSDB18-HQ **offline only** (dataset license is research/non-commercial; never redistribute).

### 5.4 Inference runtime

**ONNX Runtime (recommended).** MIT, first-class C++ API, execution providers for CoreML (macOS), DirectML/CUDA (Windows), CUDA/CPU (Linux), thread control and memory planning; prebuilt binaries for all targets. Cost: ~30–60 MB per platform, plugin/EP packaging.

**NCNN (alternative).** BSD-3, very small, good ARM/Vulkan CPU performance, but fewer transformer operators and a model-conversion step; attractive for Android/mobile later.

**demucs.cpp (fallback).** MIT, CPU-only, ggml/Eigen, no ORT dependency, ~4 min per track with 4 threads; good for "no model runtime" builds and low-end machines, not for GPU.

**LibTorch (reject).** Hundreds of MB, ABI/toolchain coupling, overkill.

**Cloud (reject).** Violates offline/private DJ use and adds latency/cost.

**Recommendation:** ONNX Runtime; keep the separator behind an interface so demucs.cpp can be a compile-time fallback.

### 5.5 Separation timing

**Offline background queue (recommended).** Separate during/after import or on demand, in a dedicated low-priority worker pool, with progress, pause/cancel and resume. Matches Traktor (which pre-computes stems), gives the best quality, and keeps the RT thread clean.

**On-demand when a track is loaded.** Nice for "separate this one now", but the first play is delayed by 40 s–3 min; use it as a secondary trigger, not the default.

**Real-time/streaming.** 7.8 s model windows, 0.5–2 s practical latency, degraded quality, GPU required; not competitive with pre-computed stems. Defer to a research spike.

### 5.6 Cache format

> Frozen by [`docs/adr/0002-stem-cache.md`](adr/0002-stem-cache.md): one 8-channel FLAC plus a
> JSON sidecar in per-track generation directories, schema revision 41, 16-bit default, no
> default cache cap, and best-ready-model fallback on playback.

**8-channel FLAC + JSON sidecar at the original sample rate (recommended).** One lossless 8-ch file holds 4 stereo stems (FLAC supports 8 channels); JSON holds labels/colors/model/version/checksums. Read by the existing 8-ch engine path; no re-encode, no quality loss, ~50–120 MB per 4-min track. Resample model output back to the track's rate so frame positions stay aligned.

**NI `.stem.mp4` (interop option).** Reuses the existing reader/waveform path and is Traktor-compatible, but AAC/ALAC encoding adds time and is lossy for AAC; libmp4v2 is already vendored. Use as **export**, not the working cache.

**4× WAV (reject for cache).** Simple but 4–8× larger.

**In-memory only (reject).** 4 stems × 4 min stereo float32 ≈ 340 MB per deck.

### 5.7 4-stem waveform

Market practice is split between **stacked lanes** (surgery, karaoke-style stem work) and **tinted composites** (small deck heights, overview). VirtualDJ and Serato tint the waveform; Traktor's Stem Deck shows a stem view; users report that a single tinted wave is easier to read when the deck is short, while stacked lanes are better for precise stem work. Colour alone is not enough: roughly 1 in 12 men are color-blind, and users complain about dark hues and stem colors that ignore the waveform mode. Ship **both modes**, keep muted stems visible but dimmed, and pair color with labels/position.

**Option 1 — First-class lanes + tinted composite in the existing renderer (recommended, S–M).** Promote stacked mode: fixed lane heights with gaps, per-stem label and solo/mute styling, dimming when muted, per-stem gain scaling, downbeat accents; keep the tinted overlay mode as a selectable alternative. Reuses the dual-backend renderer and settings plumbing.

**Option 2 — Per-stem stereo (M–L).** Extend `WaveformData` with L/R per stem (waveform cache version bump, ~2× memory and vertices) and mirror-draw each lane. Real channel detail, but a bigger change and more GPU cost.

**Option 3 — Four independent waveform widgets (M–L).** Max layout freedom (per-stem zoom/marks) but 4× renderers/GPU and heavy skin rework.

**Recommendation:** Option 1 for the program, Option 2 as a follow-up if stem stereo detail is requested. Also fix the `kPixelPerStrip = 2` precision workaround by batching per-stem geometry with a dedicated material.

### 5.8 UI direction

Current market patterns: Traktor Pro 4 uses right-click offline separation with a per-stem mute/solo Stem Deck; Serato adds a pre-computed "Stems crate" with instant acapella/instrumental and per-stem echo/brake pads; rekordbox offers per-stem gain and STEM FX; djay's Neural Mix is the lightest CPU and the 2025 quality leader; VirtualDJ exposes five stems and stem-EQ modes. The common denominator is a **4-row stem strip per deck** (gain, mute, solo), instant complement buttons, visible cache/analysis status, and a waveform that reads at a glance.

**Option 1 — QML design system + new skin, hybrid shell (recommended, M–L).** Formalise `res/qml` components as a skinning API (tokens: color, type, spacing, radius, motion), build a new "Paramix" QML skin (deck stem lanes, collapsible tinted composite, stem mixer strip with gain/mute/solo/QuickFX, acapella/instrumental buttons, separation status, model manager), keep the legacy widget library/preferences embedded until they are ported. Incremental, keeps Deere/Tango/Shade alive, and gives the stem UI room to breathe. Add MIDI-learn mappings for the stem strip and a performance meter.

**Option 2 — Refresh the widget skins only (S).** Add stem controls/styles to Deere/Tango/LateNight. Cheapest, but the deck layout and workflow stay 2010-era and the QML path stagnates.

**Option 3 — Full QML rewrite (L).** One framework, best HiDPI/accessibility story, but must port library, preferences, dialogs and controller screens; high regression risk before stems are proven.

**Recommendation:** Option 1, staged S→M: first a tinted composite + 4-row stem strip (lowest risk, immediate value), then collapsible stacked lanes and per-stem FX sends. Define a color-blind-safe stem palette plus text labels (never color alone): drums `#E85D4A`, bass `#7A5AF8`, vocals `#2BB673`, other `#F2C94C`; validate contrast; add `qsTr()` and `Accessible.*` from day one.

### 5.9 Model hosting and distribution (decided: download-only via GitHub Releases)

**What ships:** no model weights in git or in any installer. The app carries ONNX Runtime (the inference library) and a small **model manifest** (JSON) listing model id, version, URL, SHA-256, byte size, license, stem layout, sample rate and required ORT opset. The actual `htdemucs_ft` assets (~630 MB fp16) live as GitHub Releases assets on the dedicated **`paramix-models`** repository (decision 9).

**First-use flow:** the first "Separate" action (or Settings → Stems → "Download model") shows size and the MIT license, then downloads to `<app-data>/models/htdemucs_ft/<version>/` with progress, resume, SHA-256 verification and an atomic rename. Settings offers "Install from file" for offline machines, and a "Pre-download" button so a DJ can prepare before a gig. No audio ever leaves the machine.

**Why this choice:** it keeps four platform installers small (Windows, Linux, macOS arm64, macOS x86_64), decouples model updates from app releases, avoids bundling a 630 MB bag into every build, and uses free, versioned GitHub Releases. It also keeps the door open to a CDN later by changing one manifest URL.

**Mitigations for its weaknesses:**
- *Network needed on first use* — pre-download in settings, clear ETA, offline "install from file", and an optional offline bundle installer later.
- *GitHub throttling/availability* — manifest supports an ordered list of mirror URLs; SHA-256 makes mirrors safe; switching to a CDN needs no app update.
- *Corporate proxies* — honour Qt/system proxy settings; surface actionable errors.
- *Model changes invalidate caches* — the cache key includes model id + version; old caches are GC'd; a model update prompts rather than silently re-separating.
- *Licence hygiene* — only MIT/Apache weights are hosted; `htdemucs_ft` is MIT (retain `LICENSE`/NOTICE). Unlicensed RoFormer/MDX23C/UVR weights are never hosted; user-supplied "Max" models stay local.

**Release process:** a CI job exports/quantises the model, writes `manifest.json` + `.sha256`, and uploads the asset (GitHub allows up to 2 GB per asset). The manifest is signed and pinned in the app release; the app refuses assets whose checksum or license metadata does not match.

---

## 6. Target architecture (Option B)

### 6.1 System view

```
 Library / UI actions            Analysis queue / import
 (Separate, progress, badges)    (auto-separate setting)
             │                            │
             └──────────────┬─────────────┘
                            ▼
                 ┌─────────────────────────┐
                 │ StemService             │  owned by CoreServices
                 │  job queue (persistent) │
                 │  model manager          │
                 └───────┬─────────┬───────┘
                         │         │
             ┌───────────▼──┐   ┌──▼──────────────┐
             │ StemSeparator│   │ StemCache       │
             │ ONNX Runtime │   │ 8ch FLAC + JSON │
             │ CPU/CoreML/  │   │ atomic + LRU GC │
             │ DirectML/CUDA│   └──┬──────────────┘
             └───────┬──────┘      │
                     │             │
                     ▼             ▼
             ┌──────────────────────────────┐
             │ SoundSourceProxy hook        │
             │ → SoundSourceStemCache (8ch) │
             └──────────────┬───────────────┘
                            ▼
             ┌──────────────────────────────┐
             │ EngineDeck::processStem()    │  existing path
             │ 4 × gain/mute/FX/VU → mix    │
             └──────────────┬───────────────┘
                            ▼
             ┌──────────────────────────────┐
             │ WaveformRendererStem         │
             │ 4 lanes + downbeat accents   │
             └──────────────────────────────┘
```

### 6.2 Bar and downbeat information (ADR-003)

The grid format is extended **additively under the existing `BeatGrid-2.0` / `BeatMap-1.0`
version strings**; see [`docs/adr/0003-grid-format.md`](adr/0003-grid-format.md). There is no
`BeatGrid-3.0`, no dual-write and no new column.

```proto
message BarInfo {
  optional uint32 beats_per_bar = 1;      // e.g. 4
  optional uint32 first_beat_in_bar = 2;  // zero-based ordinal of the bar-start beat
}

message BeatGrid {
  // existing fields unchanged
  optional BarInfo bar = 3;               // next free tag in the existing message
}

message BeatMap {
  // existing fields unchanged
  optional BarInfo bar = 3;               // next free tag in the existing message
}
```

- **Absence means unknown, never 4/4.** A grid without `bar` behaves exactly as it does today.
- **Phase is relative to the grid anchor**, not an absolute downbeat frame, so translation and
  scaling preserve it.
- **Downgrades are safe.** Older builds ignore unknown protobuf fields and keep reading the
  grid. Changing the version string is what triggers the destructive frame-0 placeholder in
  `trackdao`, which is why the version strings do not change.
- **Malformed bar info is ignored** with a warning; the grid is never discarded because of it.
- **No schema revision and no migration:** the grid lives in `library.beats`.
- **Detection result** from a new `BeatDetector` interface: beats, downbeat positions,
  confidence, tempo segments, detector id, hypotheses (half/double, phase alternatives).
- **Builder:** `BeatGridBuilder` converts beats → `Beats` markers (piecewise tempo), picks the
  bar phase, resolves half/double using downbeat periodicity + tempo prior, and records
  confidence.
- **Confidence and detector id** stay in `beats_sub_version` for now; promoting them to real
  fields is a separate later decision.
- **Consumers:** downbeat lines in the waveform (thick/colored); optional bar quantise,
  bar-length loops and downbeat-aware sync (feature-flagged); grid editor gains "set downbeat",
  "shift grid", hypothesis choice, confidence badge; `trySetBpm` becomes non-destructive
  (warn + keep map) or explicitly "flatten grid".
- **Fallback:** if the ML model is unavailable, Queen Mary runs and the grid has no bar info.
- **Prerequisite:** fix the `Beats::operator==` sample-rate comparison bug (`beats.h:216-221`,
  it uses `&&`), otherwise equality-based round-trip tests are vacuous.

### 6.3 Separation pipeline

1. **Trigger:** import scan (optional auto-separate), context menu "Separate stems", analysis dialog, or "Separate now" on a loaded track.
2. **Job:** open the track with `SoundSourceProxy(track).openAudioSource()` off the RT thread; decode to the model rate (44.1 kHz) with `libswresample`; mono/stereo as the model expects.
3. **Inference:** chunked windows (typically 7.8 s, 25% overlap), Hann/linear overlap-add; optional test-time augmentation/ensemble for the Max tier; ORT session per worker, reused.
4. **Output:** 4 stems, resampled back to the track's rate, normalized consistently; write 8-channel FLAC atomically (temp dir → rename) plus `meta.json`.
5. **Peaks:** feed the 8-channel buffer through `AnalyzerWaveform` (or compute peaks inline) so `Waveform-6.1` stem data exists and the renderer lights up.
6. **Publish:** update DB cache row, `Track::setStemInfosWhileLocked()` (labels/colors), emit progress/finished, refresh library badge.
7. **Cancel/pause:** ORT run is chunk-granular; check a cancellation flag between chunks; persist progress for resume.

### 6.4 Playback integration

- Add `SoundSourceStemCache` (a `SoundSource`/`AudioSource` wrapper) that opens the cache FLAC with 8 channels and reports the original track's duration/rate.
- Hook it in `SoundSourceProxy::openAudioSource()` (it already holds the `TrackPointer`) **before** provider selection: if `StemCache::lookup(track)` succeeds and the cache is valid, return the stem source; otherwise fall back to the original file. This covers both `CachingReaderWorker` and `AnalyzerThread`.
- `CachingReaderWorker` already requests the maximum supported channels (8) for decks; the engine's `processStem()` path triggers on channel count > 2, so **no engine rewrite is needed**.
- Populate `StemInfo` from cache metadata on load (and persist in DB for library display), so `.stem.mp4`-specific JSON parsing is not required for generated stems.
- Keep `.stem.mp4` reading untouched; add export later by muxing 4 ALAC/AAC streams + the NI JSON atom with FFmpeg/libmp4v2.

### 6.5 Waveform changes

- Extend `allshader::WaveformRendererStem`: fixed 4-lane layout with configurable height/gap, per-stem label text, solo/mute dimming, per-stem gain scaling, downbeat accents, optional combined overview.
- Fix the scene-graph `kPixelPerStrip = 2` precision workaround by batching stem geometry per lane and/or using a dedicated material; keep the same shader for both backends.
- Target: < 8 ms CPU per frame for 4 decks × 4 stems at 1080p; no per-frame allocation; dirty-only uploads.
- Optional follow-up: per-stem stereo (Waveform cache v7) for genuine L/R lanes.

### 6.6 UI changes (new QML skin "Paramix")

- **Deck:** waveform stack (overview + 4 stem lanes or tinted composite), stem mixer strip (4 faders, mute/solo, QuickFX knob, color chip), instant acapella/instrumental buttons, separation badge ("Ready · htdemucs_ft" / "Separating 42%" / "Not separated").
- **Library:** stem status column/badge, context actions (Separate, Separate selected, Re-separate, Open cache, Export `.stem.mp4`), model/quality selection, storage usage.
- **Settings:** new "Stems" page (on-demand default, batch option, quality tier, device CPU/GPU, cache limit, pre-download model, install-from-file, license) and beatgrid options (detector, downbeat preferences, reanalysis policy).
- **Design system:** tokens in `res/qml/Theme` + per-skin overrides; dark default; contrast-checked; 1280×720 minimum; HiDPI; `qsTr()` from day one; `Accessible.*` on interactive controls.
- **Customisation contract** (what "highly customizable" means): (1) design tokens for color/typography/spacing/radius; (2) layout variants (2/4 deck, stem lane density, stacked vs tinted); (3) per-deck options (which controls show, waveform height); (4) named color schemes, including a color-blind-safe default; (5) user skin directories and QML component overrides loaded at runtime without recompiling; (6) all customization persisted in settings and portable with the profile.
- **Compatibility:** the Paramix QML skin is the default from the first release; legacy widget skins (Deere, LateNight, Tango, Shade) remain selectable; the legacy library/preferences widgets are embedded until ported, so functionality never depends on a QML rewrite.

### 6.7 Key interfaces (sketch)

```cpp
// src/stems/stemseparator.h
class StemSeparator {
  public:
    virtual ~StemSeparator() = default;
    virtual bool initialize(const ModelSpec& spec, QString* error) = 0;
    virtual bool separate(const AudioSource& source,
                          const std::atomic<bool>& cancel,
                          StemProgressCallback progress,
                          StemBuffers* out) = 0;
};

// src/stems/stemcache.h
class StemCache {
  public:
    static std::optional<StemCacheEntry> lookup(const Track& track);
    static bool write(const Track& track, const StemBuffers&, const ModelSpec&, QString* error);
    static void evictToLimit(qint64 maxBytes);
};

// src/analyzer/beatdetector.h
struct BeatDetectionResult {
    QVector<mixxx::audio::FramePos> beats;
    QVector<int> downbeatIndices;
    float confidence;
    QString detectorId;
    int meter;
};

class BeatDetector {
  public:
    virtual bool initialize(QString* error) = 0;
    virtual BeatDetectionResult detect(const AudioSource&, const std::atomic<bool>& cancel) = 0;
};
```

### 6.8 Packaging & build

- CMake: `STEMS` (default ON when ORT found), `STEMS_ORT_ROOT`, `STEMS_GPU` (CoreML/DirectML/CUDA), `BEAT_ML`; all feature-flagged so distro builds without models still compile.
- **No model weights are bundled.** ONNX Runtime binaries ship with the app. At runtime the app reads the pinned manifest and downloads `htdemucs_ft` (~630 MB fp16) from GitHub Releases into the per-user models directory, with SHA-256 verification, resume, license display and an "Install from file" path for offline machines.
- Targets: Windows x64; Linux x64 (arm64 where ORT/EP support allows); macOS **universal** (arm64 + x86_64) with universal ORT dylibs, codesigned and notarised. Android/iOS out of scope for v1.
- macOS: CoreML EP, hardened runtime, notarisation; Windows: DirectML optional and no hard CUDA dependency; Linux: CPU/CUDA, Flatpak/AppImage notes (network permission for the model download; packagers may preinstall via install-from-file).
- CI: GitHub Actions matrix (macOS arm64+x86_64, Windows, Ubuntu), build with and without stems, unit tests; model-dependent tests use mock separators; a separate release job publishes model assets + manifest + SHA-256.

### 6.9 Rebrand to Paramix

- **Product identity:** application name `Paramix`, icons/splash/wordmark, About dialog, window titles, installer metadata, package IDs (`org.paramix.Paramix` or similar), desktop entry, Flatpak ID, macOS bundle identifier, Windows product/upgrade GUIDs.
- **UI copy:** new skin name, settings pages, library labels, tooltips, website/docs links; keep "Traktor" and "Native Instruments" out of product copy (interoperability is described as "Stem file export", not endorsement).
- **Legal:** retain GPL-2.0 notices, upstream copyright headers and attribution to the Mixxx project; add a NOTICE explaining the fork. Do not use Mixxx trademarks as the product name.
- **Data/compatibility decision (resolved 2026-11-11):** move to **`~/.paramix`** with a first-run migration wizard (decision 10). The wizard copies the library DB, settings and controller mappings, verifies the copy before switching, and leaves `~/.mixxx` untouched so a downgrade or a failed migration is recoverable. Internal `Mixxx` identifiers may stay where changing them would break file formats; user-visible paths, settings and branding become Paramix.
- **Versioning:** `Paramix <fork-version>` surfaced in the UI, with the upstream Mixxx version recorded in About/diagnostics (e.g. "Paramix 1.0 (based on Mixxx 2.7.0)").

---

## 7. Multi-agent execution plan

### 7.1 Agent roster

| Agent | Mission | Owns (directories/files) | Primary outputs |
|---|---|---|---|
| **A0 Architect/Integrator** | ADRs, interfaces, merge gate, nightly integration | `docs/adr/`, `CMakeLists.txt`, contract headers | ADRs, frozen interfaces, green integration branch |
| **A1 BeatGrid core** | Bar info, serialization, builder, editor semantics | `src/track/beats.*`, `src/track/beatfactory.*`, `src/track/beatutils.*`, `src/proto/beats.proto` | ADR-003 bar info + tests |
| **A2 Beat ML** | Detector plugin, ONNX model, accuracy evaluation | `src/analyzer/plugins/beatml/`, `tools/beatml/` | Beat This! plugin + F-measure report |
| **A3 Stem runtime** | ORT separator, model manager, chunking | `src/stems/` (separator/model), `tools/stems/` | Working separator + benchmarks |
| **A4 Stem pipeline/DB** | Cache, jobs, schema, DAO, source wrapper | `src/stems/stemcache.*`, `src/library/dao/`, `res/schema.xml`, `src/sources/soundsourcestemcache.*` | Durable cache + migration |
| **A5 Audio engine** | Playback integration, controls, latency | `src/engine/` (incl. `cachingreader/`), `src/mixer/` | 8-ch playback verified, no xruns |
| **A6 Waveform** | 4-lane renderer, downbeats, performance | `src/waveform/`, `src/rendergraph/`, `res/shaders/` | 60 fps 4-deck stem waveform |
| **A7 QML/UX** | Design system, Paramix skin/branding, customization contract, settings, status | `res/skins/Paramix/`, `res/qml/`, `src/qml/`, `src/preferences/dialog/dlgprefstems.*`, branding assets | New default skin + Stems settings |
| **A8 QA/Validation** | Fixtures, SDR/F-measure, perf, soak | `src/test/`, `tools/ci/`, benchmark harness | Automated gates + human checklist |
| **A9 Compliance/Packaging** | Licences, NOTICE, model release pipeline, installers, CI, rebrand metadata | `packaging/`, `.github/`, `docs/third-party-models.md` | Signed builds, license audit, GitHub model releases |

**Operating rules:** one worktree/branch per agent; no edits outside owned paths without an interface change approved by A0; every change lands with tests; A0 runs an automated review pass before merge; a human reviews audio-affecting merges and signs off releases.

### 7.2 Dependency graph

```
A0 ──► contracts ──► A1, A2, A3, A6, A7
A2 (beat model) ─────► A1 (builder uses detector results)
A3 (separator) ─────► A4 (cache/queue) ─────► A5 (playback) ─────► A6 (waveform)
A4 ─────────────────► A7 (status/settings/library badges)
A8 ── parallel from day 1 (fixtures, harness) ──► gates
A9 ── parallel from phase 2 (licenses, packaging, CI)
```

Critical path: **A3 → A4 → A5 → A6 → integration**. A1/A2 and A7 can run widely in parallel; A7's deck UI depends on A6's lane contract, not its implementation.

### 7.3 Phases, waves and gates

| Phase | Duration | Agents | Contents | Exit gate |
|---|---|---|---|---|
| **−1 Baseline** | 1 wk | A0, A8 | **DONE 2026-11-11** (`91b72cd`, `7cbd5fc`). Commit the import; restore `.clang-format`, `.pre-commit-config.yaml`, `.gitignore`, `.github/workflows`; green build/tests on all four targets (Windows x64, Linux x64, macOS arm64, macOS x86_64). No `upstream` remote — the owner's instruction is not to sync with upstream. Measured results in `docs/baseline-report.md`; run `43100769404` green on all five jobs | ✅ CI green, formatting gated |
| **0 Discovery & spikes** | 2 wks | A0, A2, A3, A6, A8 | ADR-001 runtime/model, ADR-002 cache format, ADR-003 grid format, ADR-004 UI/customization, ADR-005 model distribution; spikes: ONNX `htdemucs_ft` on M-series/Windows/Linux; Beat This! ONNX F-measure; 8-ch FLAC write/read; 4-lane 60 fps prototype; GitHub Releases download dry run. **Stem-layer inventory done** (§3.2, §3.3) and **beatgrid audit done** (`docs/beatgrid-audit.md`); measure the in-tree qm-dsp `DownBeat` baseline before evaluating an ML downbeat model | Measured numbers + ADRs accepted |
| **1 Contracts** | 1 wk | A0 + all | Freeze `StemSeparator`, `StemCache`, `BeatDetector`, the ADR-003 bar-info grid format, renderer/skin contracts; CMake flags; schema draft; manifest format; fixtures | Interfaces reviewed, skeleton builds |
| **2.1 Build** | 2 wks | A1–A3, A6–A9 | ADR-003 bar-info core + serialization; Beat This! plugin; separator core + dummy model; **design tokens + Paramix skin shell + branding inventory**; license matrix. Drop "renderer lane prototype": `WaveformRendererStem` exists (§3.3) | Unit tests green; demo of separated fixture |
| **2.2 Build** | 2 wks | A3–A7 | Cache/DB/jobs (single + batch); `SoundSourceStemCache`; playback verification; **wire the existing stem renderer to separated sources** (it already handles `.stem.mp4` inputs) plus tinted composite mode; stem mixer/status UI; GitHub Releases model download manager | End-to-end: separate → load → 4 stems visible/mixable |
| **2.3 Build** | 2 wks | A1, A5, A7, A9 | Downbeats/editor + bar behaviors (v1); `.stem.mp4` export; xrun/latency tuning; settings/library badges; rebrand packaging, installers, signed manifest | Feature-complete, cross-platform builds |
| **3 Integration** | 2 wks | A0, A8 | Merge freeze, cross-platform QA, cache GC, error paths, upgrade/downgrade tests, Traktor export round-trip | All gates green on all four targets |
| **4 Hardening** | 2–3 wks | A8 + all | Soak (2 h), CPU/GPU matrices, SDR/F-measure report, human DJ sessions, docs | Beta sign-off |
| **5 Release** | 1 wk | A0, A9 | Release notes, licenses, installers, model assets | Shipped |

Total ≈ **20–24 weeks** with 6–8 agents active in phases 2–3. v1 scope explicitly includes downbeats/bars, `.stem.mp4` export and the download-only model manager, per the locked decisions.

### 7.4 Parallelisation mechanics

- **Contract freeze at end of Phase 1** is the key to parallelism; after that, agents only change their own directories.
- **Nightly integration branch** built and tested by A0/A8; agents rebase daily.
- **Feature flags** (`STEMS`, `BEAT_ML`, `STEM_UI`) keep `main` usable while work lands.
- **Two review layers:** a peer agent review (diff, tests, style) + a human review for audio/DSP changes.
- **Shared-file policy:** `CMakeLists.txt`, `res/schema.xml`, `src/track/track.h`, `src/engine/engine.h` are single-owner files; changes go through A0.
- **Definition of done** per workstream: unit tests, golden/regression tests, benchmark within budget, docs, no new license, and a demo artifact.

---

## 8. Verification and metrics

### 8.1 Test pyramid

- **Unit (GTest):** ADR-003 bar-info round-trip, malformed-input and downgrade simulation; detector result → builder → grid; downbeat phase; cache atomicity/GC; chunk overlap-add math; source wrapper metadata; model-manager checksum.
- **Golden audio:** synthetic 4-band mix for plumbing (identity separator); NI `.stem.mp4` fixtures already in `src/test/stems` for playback; 5–10 CC-licensed tracks with hand-labeled beats for F-measure; MUSDB18-HQ dev-only for SDR (never committed).
- **End-to-end:** import → separate → load → assert 8 channels, 4 stem controls, waveform stem data, mute/solo, unload/reload, library move.
- **Regression:** old library opens; downgrade build reads the dual-written 2.0 grid; Serato import/export unchanged; `.stem.mp4` playback unchanged.
- **Performance:** separation wall time and peak RSS per model/hardware; playback xrun count at 128-frame buffers; waveform frame time at 4 decks; cache size.

### 8.2 Targets

| Metric | Target |
|---|---|
| Beat F-measure | ≥ 0.90 electronic, ≥ 0.85 mixed; no regression vs Queen Mary on existing set |
| Downbeat F-measure | ≥ 0.75 |
| Separation SDR (HTDemucs default) | ≥ 9.0 avg MUSDB; vocals ≥ 8.0 |
| Separation time (4-min track) | ≤ 150 s on M-series CPU with `htdemucs_ft` (~2 min; Traktor/iZotope RX is ~30–60 s); ≤ 30 s with CoreML/DirectML; ≤ 5 min worst-case CPU |
| Playback | 0 xruns in 2 h soak, 4 decks, 128-frame buffer; 10–20 ms total latency |
| Waveform | ≤ 8 ms CPU/frame, 60 fps, 4 decks |
| Memory | < 1.5 GB RSS with two separation workers |
| Cache | 8-ch FLAC at the source rate, ≈ 85–120 MB for a 4-min track at 16-bit (ADR-002); no cap by default, optional user cap with LRU eviction, free-space preflight |

### 8.3 Human validation

Two DJ sessions per milestone: one technical (beat alignment, stem bleed, FX, latency) and one performance-style (30-min mix with stems, controller). Log issues against acceptance criteria; no release without sign-off.

---

## 9. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Model weights license (RoFormer/MDX23C/UVR unlicensed; Open-Unmix `umxl` NC) | Legal/distribution blocker | Bundle only MIT/Apache weights (HTDemucs, Beat This!); user-supplied models behind a license gate; audit in `docs/third-party-models.md` |
| Apache-2.0/TF, AGPLv3, GPLv3 deps (Spleeter, Essentia, aubio) | GPLv2 incompatibility | Reject; document why; keep evaluation-only tools out of the build |
| Separation too slow on CPU | UX failure | Default ONNX+CoreML/DirectML; fast tier; background queue; show ETA; don't block playback |
| CPU/thermal spikes during playback | Audio dropouts | Separation always low-priority; limit workers; pre-compute; never decode stems on RT thread |
| 8-ch FLAC streaming glitches | Playback artifacts | Prefetch/LRU tuning; sequential layout; test HDD/network drives; fallback to premix |
| Phase/comb filtering when stems are summed after gain/fade | Audible artifacts | Keep per-stem gain/mute in the linear mix; avoid independent per-stem resampling; A/B against the original; document a "sum to original" check |
| Model update invalidates every cache entry | Re-separation storm | Cache key includes model id + version; migrate old entries lazily; warn in UI; keep previous cache until space pressure |
| Schema/downgrade data loss | User grids destroyed | Additive fields under unchanged version strings (ADR-003); round-trip and downgrade tests; backup on upgrade |
| QML skin scope creep / library parity | Schedule slip | Hybrid shell: keep legacy library/prefs; freeze skin scope at deck/mixer/settings |
| Waveform GPU cost (48 verts/px) | Frame drops | Lane batching, dirty-only uploads, precision fix, per-deck budget |
| Fork baseline (no CI, missing dotfiles) | Agents can't verify | Phase −1 mandatory |
| Model download UX (proxy, offline) | First-run failure | Resumable download, install-from-file, clear errors, optional offline bundle |
| "Commercial-grade" expectation mismatch | Perceived failure | Publish SDR/benchmarks, quality tiers, A/B preview, honest docs |

---

## 10. Licensing and compliance checklist

1. **Code:** ONNX Runtime MIT; demucs-onnx MIT; demucs.cpp MIT; NCNN BSD-3; FFmpeg LGPL (existing); libFLAC BSD (existing); Qt LGPL/GPL (existing). No new GPL-incompatible links.
2. **Weights:** HTDemucs v4 MIT (code + weights); Beat This! MIT (code + weights); Open-Unmix `umxhq` MIT only; RoFormer/MDX23C/UVR weights **not hosted or bundled** (unlicensed); `umxl` **not** used (CC BY-NC-SA).
3. **Artifacts:** `docs/third-party-models.md`, NOTICE updates, license text shown at download, SHA-256, version pinning.
4. **Datasets:** MUSDB18-HQ only for offline evaluation (research license); never ship audio or derived weights.
5. **Trademark:** do not use "Traktor" or Native Instruments marks in product copy; describe as "stem separation".
6. **User content:** generated stems are derivative audio of the user's own files; cache stays local; no upload.
7. **Export:** the required `.stem.mp4` export must respect the NI Stems format spec and the user's own rights.

---

## 11. Decision log and remaining questions

### Resolved 2026-11-11

| # | Decision |
|---|---|
| 1 | Default separator `htdemucs_ft` (single `htdemucs` only as low-end fallback). |
| 2 | On-demand separation with an optional batch mode; no auto-separate on import. |
| 3 | Downbeats/bars are required in v1. |
| 4 | Traktor interoperability required (`.stem.mp4` export in v1). |
| 5 | New QML skin is the default and highly customizable. |
| 6 | Windows x64, Linux x64 (+ arm64 where feasible), macOS arm64 + x86_64. |
| 7 | Models hosted on GitHub Releases, download-only (nothing in installers/git). |
| 8 | Rebrand as **Paramix**. |
| 9 | Model release assets live in a dedicated **`paramix-models`** repository. |
| 10 | Migrate the data directory to **`~/.paramix`** with a first-run wizard and a downgrade path. |
| 11 | Version from **`0.1`** with milestone releases. |
| 12 | GPU acceleration **auto-enabled with a CPU opt-out**. |
| 13 | `.stem.mp4` codec is **user-selectable (ALAC or AAC)** in Settings. |
| 14 | **Phase −1 complete** on 2026-11-11 (`91b72cd`, `7cbd5fc`): all four build targets green, repository checks enforced, formatting gated. See `docs/baseline-report.md`. |
| 15 | **No `upstream` remote** and no syncing with mixxxdj/mixxx; the imported tree is the source of truth. The plan's Phase −1 row previously said to add one — that instruction is withdrawn. |
| 16 | **Stem-layer inventory:** per-stem waveform rendering, stem controls and `.stem.mp4` reading already exist, so O3 and part of O2 are integration work, not construction (§1, §3.2, §3.3). |
| 17 | **Beatgrid audit complete** (`docs/beatgrid-audit.md`): no downbeat field exists; qm-dsp's `DownBeat` is compiled but unused; grids must be extended **additively with unchanged version strings**, because an unknown version makes older builds replace the grid with a frame-0 placeholder and save it. ADR-001 is not a prerequisite for downbeats. |

### Still open (defaults I will assume if you do not object)

1. **Supported OS floor:** *Default: macOS 13+, Windows 10 21H2+, Ubuntu 22.04+/Fedora 39+.* (GPU auto-enable is already decided; only the floor and the Linux distro matrix remain.)
2. **Max-quality tier:** user-supplied RoFormer in v1 or v1.1? *Default: v1.1.*
3. **Customisation depth:** tokens/layout variants only, or full runtime QML overrides with a stability promise? *Default: full overrides, documented as experimental for one release.*
4. **Name/domain/icon clearance** for Paramix and upstream attribution text. *Action: confirm before Phase 5 packaging.*
5. **Telemetry/crash reporting:** none by default? *Default: none; opt-in later.*

---

## Appendix A — First 10 working days (concrete backlog)

**Days 1–2 (A0, A8):** commit the import on a branch; restore upstream dotfiles/CI; add `docs/`, `.gitignore` entries for `build/`, `models/`, `stems/`; run `ctest` on all four targets (Windows x64, Linux x64, macOS arm64, macOS x86_64); record baseline.

**Days 3–5 (spikes):**
- A3: export `htdemucs_ft` to ONNX; run a 4-min track on M-series CPU + CoreML and on Windows CPU; record wall time/RSS/SDR. Dry-run the GitHub Releases manifest/download path.
- A7/A9: branding inventory (name, icons, bundle IDs, desktop entry) and the model-manifest ADR.
- A2: run Beat This! ONNX on 10 labeled tracks; compute F-measure vs Queen Mary.
- A4: write/read an 8-channel FLAC; verify `SoundSourceFLAC` 8-ch playback through `EngineDeck`.
- A6: prototype 4-lane renderer geometry and measure frame time.

**Days 6–8 (contracts):** ADRs 001–005 (runtime/model, cache, grid, UI/customization, model distribution); `src/stems/` headers; ADR-003 `BarInfo` proto; schema draft; model manifest schema; `docs/third-party-models.md` skeleton.

**Days 9–10 (skeleton):** CMake flags; `StemCache` read/write with a dummy separator; ADR-003 bar-info serialization test; design tokens + `res/skins/Paramix/skin.ini` + `main.qml` shell with Paramix branding; nightly CI.

---

## Appendix B — File-level touch map

| Area | Files (primary) |
|---|---|
| Beat model/serialization | `src/track/beats.{h,cpp}`, `src/track/beatfactory.{h,cpp}`, `src/track/beatutils.{h,cpp}`, `src/proto/beats.proto` |
| Beat detection | `src/analyzer/analyzerbeats.{h,cpp}`, `src/analyzer/plugins/beatml/*`, `src/analyzer/plugins/analyzerplugin.h` |
| Beat persistence | `res/schema.xml`, `src/library/dao/trackdao.cpp`, `src/library/dao/trackdao.h`, `src/track/trackrecord.h` |
| Beat UI | `src/library/dlgtrackinfo.{h,cpp}`, `src/widget/wtrackmenu.cpp`, `src/waveform/renderers/allshader/waveformrenderbeat.cpp` |
| Stems service | `src/stems/*` (new), `src/coreservices.{h,cpp}`, `src/analyzer/trackanalysisscheduler.*` (model) |
| Stems DB/cache | `res/schema.xml`, `src/library/dao/trackdao.cpp`, `src/stems/stemcache.*` |
| Stem playback | `src/sources/soundsourceproxy.cpp`, `src/sources/soundsourcestemcache.*` (new), `src/engine/cachingreader/cachingreaderworker.cpp`, `src/analyzer/analyzerthread.cpp` |
| Stem UI/controls | `src/engine/channels/enginedeck.cpp`, `src/mixer/basetrackplayer.cpp`, `src/effects/effectsmanager.cpp`, `src/qml/qmlstemsmodel.*` |
| Waveform | `src/waveform/renderers/allshader/waveformrendererstem.*`, `src/waveform/waveform.{h,cpp}`, `src/analyzer/analyzerwaveform.cpp`, `res/shaders/` |
| Skin/UI | `res/skins/Paramix/*` (new), `res/qml/`, `src/skin/qml/qmlskin.cpp`, `src/preferences/dialog/dlgprefstems.*` (new), `src/preferences/dialog/dlgpreferences.cpp` |
| Build/packaging | `CMakeLists.txt`, `packaging/*`, `.github/workflows/*`, `docs/third-party-models.md` |
| Tests | `src/test/beat*`, `src/test/stem*`, `src/test/stemcachetest.cpp` (new), `src/test/CMakeLists` (top-level `CMakeLists.txt`) |

---

## Appendix C — References

**Open-source separation / inference**
- HTDemucs v4: https://github.com/facebookresearch/demucs (archived; bugfix fork `adefossez/demucs`)
- demucs-onnx (MIT ONNX export, <1e-3 parity): https://github.com/StemSplit/demucs-onnx
- demucs.cpp (MIT, CPU/ggml): https://github.com/sevagh/demucs.cpp
- xLights HTDemucs C++/ONNX Runtime precedent (`src-core/media/StemSeparator.cpp`): https://github.com/xLightsSequencer/xLights
- BS-RoFormer reference code (MIT): https://github.com/lucidrains/BS-RoFormer
- Community RoFormer weights (no license file — do not bundle): https://github.com/TRvlvr/model_repo
- Music-Source-Separation-Training (MIT tooling): https://github.com/ZFTurbo/Music-Source-Separation-Training
- Spleeter (MIT; TensorFlow — GPLv2 conflict): https://github.com/deezer/spleeter
- Open-Unmix (`umxhq` MIT, `umxl` CC BY-NC-SA): https://github.com/sigsep/open-unmix-pytorch
- Ultimate Vocal Remover (MIT app; community models mostly unlicensed): https://github.com/Anjok07/ultimatevocalremovergui
- ONNX Runtime (MIT): https://github.com/microsoft/onnxruntime
- NCNN (BSD-3): https://github.com/Tencent/ncnn

**Beat tracking**
- Beat This! upstream (MIT code + weights): https://github.com/CPJKU/beat_this
- Beat This! ONNX export (MIT, 82 MB, beat + downbeat logits): https://huggingface.co/aaatmy/beat-this-onnx
- All-In-One structure/beat analyzer: https://github.com/mir-aidj/all-in-one
- Paper: Foscarin, Schlüter, Widmer, "Beat this! Accurate beat tracking without DBN postprocessing", ISMIR 2024.

**DJ market / UX**
- Traktor Pro 4 review (iZotope RX offline separation): https://musictech.com/reviews/dj/native-instruments-traktor-4-pro/
- Serato Stems guide: https://www.digitaldjtips.com/using-stems-in-serato-dj-pro-all-you-need-to-know/
- rekordbox Stems FAQ: https://rekordbox.com/en/support/faq/stems-7/
- djay Neural Mix: https://help.algoriddim.com/topic/hardware/using-neural-mix-on-supported-controllers
- VirtualDJ stem/EQ modes: https://virtualdj.com/manuals/virtualdj/interface/mixer/audio.html
- 2025 stem quality comparison: https://www.digitaldjtips.com/best-sounding-stems-2025/
- Live stem latency/artifacts: https://dj.studio/blog/live-stem-separation-latency-artifacts-failsafes
