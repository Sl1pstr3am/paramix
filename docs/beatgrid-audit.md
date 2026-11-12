# Beatgrid and downbeat audit

**Date:** 2026-11-12
**Method:** two read-only source audits plus direct verification of every load-bearing claim.
**Scope:** what the imported tree provides for beat grids, what is missing for downbeats, and what
the cheapest correct path is.

Related: `docs/paramix-stem-beatgrid-ui-plan.md` (specification), `docs/agent-context.md` (handoff).

---

## Headline

1. **There is no downbeat, bar, meter or bar-phase concept anywhere in the grid.** `BeatMarker` has
   exactly two fields: `m_position` and `m_beatsTillNextMarker` (`src/track/beats.h:42-44`). The
   word "downbeat" appears in `src/` three times, all in comments.
2. **A downbeat detector is already compiled into the binary and unused.** `lib/qm-dsp/dsp/tempotracking/DownBeat.cpp`
   is listed uncommented in `CMakeLists.txt:4753`, linked into `mixxx-lib` at `:4787`, and nothing
   in `src/` references it (verified: `grep -rn "DownBeat" src/` returns nothing). This is a
   Davies & Plumbley (2006) downbeat detector that takes exactly the beat positions the Queen Mary
   plugin already produces and then throws away.
3. **The blocker is not detection, it is representation.** The grid has nowhere to store a
   downbeat, and the serializer has no way to carry one. Worse, an unrecognized grid version is
   handled destructively: the grid is replaced with a frame-0 placeholder and then written back
   over the original blob.

---

## 1. Current state

| Capability | Status | Evidence |
|---|---|---|
| Grid arithmetic (nth beat, prev/next, ranges) | Works | `src/track/beats.h:68-212`, `beats.cpp:385+`, `:768+` |
| Constant-tempo grid | Works | `m_markers.empty()` means constant tempo (`beats.h:282-284`) |
| Variable-tempo grid (tempo map) | Works | `BeatMarker{position, beatsTillNextMarker}` |
| Beat detection | Works, two plugins | Queen Mary (`analyzerqueenmarybeats.cpp`), SoundTouch (BPM only) |
| Downbeat / bar / meter | **Absent** | no field in `Beats` or `BeatMarker` |
| Downbeat detection | **Available but unused** | `lib/qm-dsp/.../DownBeat.cpp`, compiled |
| Grid editing | BPM / offset / scale only | `dlgtrackinfo.cpp:710-817`, `wtrackmenu.cpp:1478-1486` |
| Marker-level editing | **Absent** | no code constructs a `BeatMarker` from the UI |
| Beat rendering | Works, single color | `allshader/waveformrenderbeat.cpp:116-156` |
| Accent / second color | **Absent** | one uniform color, one `GeometryNode` |
| ML inference runtime | **Absent** | no ONNX/TFLite/TF/CoreML/OpenVINO anywhere |
| Bundled model precedent | **Absent** | no `*.onnx`, no model registry, no download flow |

---

## 2. The free win: qm-dsp already has a downbeat detector

`DownBeat::findDownBeats(audio, audioLength, beats, downbeats)` returns indices into the beat
array (`lib/qm-dsp/dsp/tempotracking/DownBeat.h:56-73`). Its input contract is *"beats expressed in
multiples of the df increment"*, which is exactly what `AnalyzerQueenMaryBeats::finalize()` already
holds at `analyzerqueenmarybeats.cpp:100-101` before converting to frames and discarding the rest.

Enabling it needs **no build-system, packaging or license change**: the object is already compiled
and linked, and qm-dsp is GPL-2.0 like the rest of the tree.

Three real caveats, all verified:

1. **Sample-rate collision.** `m_stepSizeFrames = sampleRate * 0.01161`
   (`analyzerqueenmarybeats.cpp:52`) is 512 at 44.1 kHz, **557 at 48 kHz**, 1114 at 96 kHz.
   `DownBeat` requires `dfIncrement % decimationFactor == 0` (`DownBeat.h:46-47`), and the decimator
   chain maxes out at factor 8 (`Decimator.h:57`). At 48 kHz this degrades to a 65536-point FFT per
   beat — correct but slow. Needs a step-size fix, not a rewrite.
2. **Meter is hardcoded 4/4.** `DownBeat.cpp:213-214` falls back to `timesig = 4`. There is no
   meter estimator in the tree and no field to store beats-per-bar.
3. **It buffers the whole track.** ~0.65 MB per minute of downsampled mono, times the number of
   analysis threads. Acceptable, but it is the first analyzer with O(track length) state.

**What this buys:** a working, packaged reference implementation of downbeats on all four targets,
today, for a day's work. **What it does not buy:** 3/4 and odd meters, or Beat This!-class accuracy.

---

## 3. The real blocker: representation and serialization

### 3.1 What the format can hold

`src/proto/beats.proto` (verified verbatim):

```proto
message Beat {
  optional int32 frame_position = 1;
  optional bool enabled = 2 [ default = true ];
  optional Source source = 3 [ default = ANALYZER ];
}
message BeatGrid {
  optional Bpm bpm = 1;
  optional Beat first_beat = 2;
}
```

There is no per-beat bar attribute. `enabled` and `source` are declared but never written or read —
proof that adding a proto field alone changes nothing.

### 3.2 Why the version string is dangerous

- `getVersion()` is **derived**, not stored: it decides from `hasConstantTempo()`
  (`beats.cpp:377-383`).
- Dispatch is hardcoded string comparison in two places: `beats.cpp:262-270` and
  `beatfactory.cpp:78-96`.
- An unknown version makes `fromByteArray` return `nullptr` (`beats.cpp:266-270`).
- `trackdao.cpp:1292-1296` then **fabricates** a frame-0 constant-tempo grid from the cached `bpm`
  column and calls `trySetBeats`, which marks the track dirty.
- The next save writes that placeholder over the original blob (`trackdao.cpp:1708-1710`).

So a **new** `beats_version` string does not merely make old builds ignore downbeats: an older
build opens the library, destroys the grid phase, and persists the damage. Since we cannot patch
already-released old builds, the downgrade blast radius is a design input, not an afterthought.

**Recommendation:** add fields **additively** and keep the existing version strings
(`BeatGrid-2.0`, `BeatMap-1.0`). An old build then parses the grid correctly, ignores the unknown
fields, and at worst loses downbeat information on save — instead of flattening the grid to frame
0. Include an explicit `beats_per_bar`/`bar_phase` field so absence is distinguishable from "4/4
starting at beat 0".

### 3.3 The serialization trap that will silently lose data

`toBeatGridByteArray` writes only `first_beat.frame_position` and `bpm` (`beats.cpp:349-361`).
If a downbeat field is added to the constant-tempo path but `getVersion()` still returns
`BeatGrid-2.0`, a new field written there is **discarded on every save** — and the existing
round-trip test (`beatstest.cpp:395-404`) compares `toByteArray()` to itself, so it cannot detect a
field the writer never emits. Two consequences: extend the writer, and add a test that asserts the
*decoded* value, not the byte symmetry.

### 3.4 Post-processing actively removes downbeat identity

`BeatUtils::makeConstBpm` re-anchors the grid with `fmod(regionStart, roundedBeatLength)`
(`beatutils.cpp:313-323`) and `adjustPhase` nudges it further (`:411-440`). Its own comment admits
the ideal anchor would be "the first proper downbeat". `retrieveConstRegions` also accepts at most
one outlier at 25 ms (`:16`, `:95-107`), and `BeatUtils::getBeats` regenerates uniform positions
for the tempo-map path (`:393-408`).

So even with correct detection, the construction step discards the answer. Any downbeat
implementation must thread phase through `BeatFactory::makePreferredBeats`
(`beatfactory.cpp:51-98`) rather than re-deriving it afterwards.

### 3.5 Analysis gating

`beats_sub_version` encodes the plugin id and options (`beatfactory.cpp:21-49`) and is compared in
`analyzerbeats.cpp:174-196`. A new plugin id therefore *can* trigger re-analysis — but
`ReanalyzeWhenSettingsChange` defaults to **false** (`beatdetectionsettings.h:40-41`), so existing
libraries keep their phase-less grids unless the user opts in.

---

## 4. Rendering: an accent is cheap

- GL path: `allshader::WaveformRenderBeat` writes one rectangle per beat with a single uniform
  color (`allshader/waveformrenderbeat.cpp:116-156`). Two viable accents: a **second
  `GeometryNode`** with its own color (the widget already composes background/signal/beat/mark
  nodes separately, `widgets/allshader/waveformwidget.cpp:62,84`), or switch to `RGBAMaterial`
  which already supports per-vertex color (`rendergraph/shaders/rgba.vert`). **No new shader is
  required.**
- Legacy QPainter path: one pen and one `drawLines` batch (`waveformrenderbeat.cpp:81-111`); a
  second color means splitting the list and drawing twice.
- Both implementations are live, plus the QML wrapper (`qmlwaveformrenderer.cpp:272-287`), so an
  accent change must be made in three places.
- The beat iterator exposes a beat ordinal (`beats.h:68-154`), so "is this beat a downbeat" is
  answerable without changing the renderer's data source.
- Dead knob worth reviving: `<BeatHighlightColor>` exists in `res/skins/LateNight/waveform.xml:192`
  with **zero readers** in `src/`.

---

## 5. ML versus DSP

The licensing question is easy: ONNX Runtime is MIT, Beat This! code and weights are MIT, both
compatible with GPL-2.0.

The packaging question is where the cost is:

| Target | Mechanism | Cost of adding ONNX Runtime |
|---|---|---|
| Linux x64 | apt only (`tools/debian_buildenv.sh`), no ORT package on Ubuntu 24.04 | New acquisition path; closest precedent is the libkeyfinder `ExternalProject_Add` |
| macOS arm64 + x86_64 | 3.7 GB SHA-pinned vcpkg bundle from `downloads.mixxx.org` | Cross-repo change in `mixxxdj/vcpkg`, rebuild, re-pin; not verifiable inside this repo |
| Windows x64 | same bundle mechanism | Same, plus deciding which execution providers ship |
| Flatpak | per-dependency modules built from source | Multi-hour, multi-GB build |

Two risks the plan does not mention:

1. **protobuf collision.** This tree links its own protobuf for grid/key/waveform schemas
   (`CMakeLists.txt:3799-3801`); ONNX Runtime's prebuilt distributions embed their own protobuf.
   Static linking two protobufs in one binary is an ODR hazard and must be proven, not assumed.
2. **No model-management precedent.** No registry, no checksum flow, no cache table. Notably
   `tools/paramix_verify.sh` rejects `models/*` as tracked junk — a deliberate guard against
   committing weights.

**Recommendation:** do the DSP path first and treat it as the specification for the integration
points (plugin output, phase-carrying construction, accent rendering). If measured accuracy on a
labelled fixture set is insufficient, ONNX Runtime becomes a second, informed decision with a
working baseline to compare against. The reverse order spends four-target packaging effort before
the feature can be evaluated at all.

---

## 6. Free wins already in the tree

- **Rekordbox import discards downbeats.** `rekordboxfeature.cpp:919-932` reads only `beat->time()`
  and feeds `fromBeatPositions`, while the format carries `beat_number` (1 = downbeat) per
  `lib/rekordbox-metadata/rekordbox_anlz.ksy:112-126`. Once the grid can store bar position, this
  becomes an import improvement for real libraries today.
- **Engine Prime export fabricates 4/4** from a cue point because the grid has no bar data
  (`engineprimeexportjob.cpp:120-167`, comment at `:128-134`). It becomes correct for free.
- **`fromBeatPositions` is lossy by construction** — it needs ≥2 beats and re-infers markers with a
  1.0-frame tolerance (`beats.cpp:175-236`). Import paths must attach downbeat data *after* marker
  construction, or they will lose it.

---

## 7. Ordered work list

Each step is blocked by the previous one.

1. **Grid model** (`src/track/beats.h`): add bar/meter state to `BeatMarker` and `Beats`;
   fix `operator==`, which compares sample rates with `&&` instead of `==` (`beats.h:216-221`) —
   otherwise new-field tests are vacuous.
2. **Serialization** (`src/proto/beats.proto`, `src/track/beats.cpp`): additive fields, keep the
   version strings per §3.2; extend both writers (`:349-370`); add a decoded-value round-trip test.
3. **Factory** (`src/track/beatfactory.cpp`): carry phase through construction instead of
   re-anchoring.
4. **Detection** (`src/analyzer/plugins/analyzerqueenmarybeats.cpp`, `analyzerplugin.h`): call
   qm-dsp `DownBeat`, decimate its audio sink from `processSamples`, extend the plugin output with
   downbeats and beats-per-bar (default empty implementation for SoundTouch).
5. **Post-processing** (`src/track/beatutils.cpp`): stop discarding the detected downbeat when
   anchoring and snapping.
6. **Rendering** (`allshader/waveformrenderbeat.cpp`, `waveformrenderbeat.cpp`,
   `qmlwaveformrenderer.cpp`): accent color via a second node or `RGBAMaterial`; revive
   `<BeatHighlightColor>`.
7. **Editing** (`dlgtrackinfo.cpp`, `wtrackmenu.cpp`, `bpmcontrol.cpp`): "set downbeat here", and
   make scale/translate preserve bar data.
8. **Import** (`rekordboxfeature.cpp`): use `beat_number`.
9. **Tests**: bar-preservation on scale/translate, legacy-read, unknown-version, DB round-trip.

---

## 8. Top risks

1. **Downgrade data loss** — an old build replaces a good grid with a frame-0 placeholder and saves
   it (§3.2). Mitigated only by keeping version strings stable.
2. **Silent field loss** — adding a field the constant-tempo writer does not emit, with a round-trip
   test that cannot see it (§3.3).
3. **Re-anchoring** — `BeatUtils` discards the detector's downbeat, so a correct detector still
   produces a wrong grid (§3.4).
4. **Fast analysis truncation** — analysis stops at 60 s in fast mode
   (`analyzerbeats.cpp:234-237`, `analyzer/constants.h:25`); a downbeat pass must be exempt or it
   sees a fifth of the track.
5. **Naming collision** — "phase" already means the sub-beat grid-to-audio offset
   (`beatutils.cpp:411-437`, `bpmcontrol.cpp:758-800`). Call the new concept bar position or bar
   phase, not phase.

---

## 9. Consequence for the plan

- **ADR-003 (grid format)** should be written against §3: additive fields, stable version strings,
  explicit meter, decoded-value tests.
- **ADR-001 (inference runtime)** is no longer a prerequisite for downbeats. It stays a decision for
  *separation*, and for a higher-accuracy downbeat model only if the DSP baseline proves
  insufficient.
- The Phase 0 "Beat This! F-measure" spike should first measure the **qm-dsp baseline** on the same
  fixture set, so the ML decision has a reference point.
