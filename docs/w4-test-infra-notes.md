# W4 — Test infrastructure and golden audio fixtures

**Date:** 2026-11-12
**Workstream:** W4 (test infrastructure and golden audio fixtures) from
`docs/parallel-work.md`.
**Owner:** Bradley's second-contributor team.

This workstream adds a deterministic, offline test harness for the beatgrid and
stem-separation layers. Before this change there was no end-to-end beatgrid
detection test, no fixture with a golden grid, and no synthetic fixture for
separation plumbing. Everything here is **additive**: new files only, no edits
to `CMakeLists.txt`, `res/schema.xml`, or any existing test or source file.

---

## 1. What was added

### Generator scripts (`tools/`, stdlib-only Python)

| File | Purpose |
|---|---|
| `tools/make_beatgrid_fixture.py` | Writes a mono 44.1 kHz click-track WAV plus a JSON "golden grid". |
| `tools/make_stem_fixture.py` | Writes a synthetic 4-band stem mix: 4 stereo stems, a stereo mix, and an 8-channel WAV. |
| `tools/check_fixtures.py` | Self-check that proves the golden-grid comparison logic end to end. |

### Fixture data (`src/test/fixtures/`, committed)

| File | Size | Contents |
|---|---|---|
| `beatgrid_128bpm.wav` | 690 KB | Mono, 16-bit PCM, 44 100 Hz, 8.0 s; a 2 kHz click on each beat. |
| `beatgrid_128bpm.json` | 1 KB | Golden grid: BPM, sample rate, channels, duration, and exact beat positions. |
| `stem_drums.wav` | 172 KB | Drums stem, stereo (200 Hz). |
| `stem_bass.wav` | 172 KB | Bass stem, stereo (60 Hz). |
| `stem_vocals.wav` | 172 KB | Vocals stem, stereo (1 kHz). |
| `stem_other.wav` | 172 KB | "Other" stem, stereo (5 kHz). |
| `stem_mix.wav` | 172 KB | Stereo mix = plain sum of the four stems. |
| `stem_8ch.wav` | 690 KB | 8 channels, interleaved drums,bass,vocals,other (1L,1R,2L,2R,3L,3R,4L,4R). |

### C++ tests (`src/test/`, new files)

| File | Contents |
|---|---|
| `src/test/beatgridfixturetest.cpp` | Loads the golden grid JSON, decodes the click track through `SoundSourceProxy`/`mixxx::AudioSource`, and scores candidate grids with a beat F-measure helper. |
| `src/test/stemfixturetest.cpp` | Decodes the 8-channel fixture and asserts `ChannelCount::stem()` (8 channels), non-silence, and mutual distinctness by RMS. |

---

## 2. Golden grid parameters

| Parameter | Value |
|---|---|
| BPM | 128.0 |
| Sample rate | 44 100 Hz |
| Channels | 1 (mono) |
| Bit depth | 16-bit PCM |
| Duration | 8.0 s = 352 800 frames |
| Beat period | 0.46875 s = 20 671.875 frames |
| Number of beats | 18 |
| Click | 220 samples (~5 ms) of a 2 kHz sine with exponential decay, amplitude 0.85 |

Beat positions in seconds are `i * 0.46875` for `i = 0..17`:

```
0.0000  0.4688  0.9375  1.4062  1.8750  2.3438  2.8125  3.2812  3.7500
4.2188  4.6875  5.1562  5.6250  6.0938  6.5625  7.0312  7.5000  7.9688
```

The `beat_positions_frames` list in the JSON carries the ideal (fractional)
positions; `click_start_frames` carries the rounded integer sample where each
click actually begins. The F-measure compares against the ideal positions, so
the sub-sample rounding is irrelevant under the ±70 ms tolerance.

---

## 3. How to regenerate the fixtures

Run from the repository root (Python 3.14 via `py`, standard library only):

```powershell
py tools/make_beatgrid_fixture.py
py tools/make_stem_fixture.py
py tools/check_fixtures.py
```

The generators are fully deterministic (closed-form math, no randomness, no
network). Re-running them reproduces byte-identical output, which is what makes
the fixtures safe to commit and to compare against in CI. The scripts locate
`src/test/fixtures/` relative to their own path, so they work from any working
directory.

---

## 4. CMake registration snippet

The two new test files must be added to the existing `set(src-mixxx-test ...)`
list inside the `if(BUILD_TESTING)` block of the root `CMakeLists.txt`
(currently around line 2871). **This document only records the snippet; it does
not edit `CMakeLists.txt`.** Ask the integrator to apply it (the file is a
shared single-owner file per `docs/parallel-work.md` §4).

Insert alphabetically:

```cmake
    src/test/beatgridtest.cpp
    src/test/beatgridfixturetest.cpp   # NEW (W4): golden click-track fixture test
    src/test/beatmaptest.cpp
```

and

```cmake
    src/test/sqliteliketest.cpp
    src/test/stemfixturetest.cpp       # NEW (W4): synthetic 4-band stem fixture test
    src/test/synccontroltest.cpp
```

Notes:

* `stemfixturetest.cpp` has **no** `STEM`-only dependency: it decodes a plain
  8-channel WAV through the generic audio-source API, and `ChannelCount::stem()`
  is always available (`src/audio/types.h`). It can therefore live in the
  unconditional list above. If you prefer to group it with the existing stem
  tests, add it to the `if(STEM)` block instead (where `src/test/stemtest.cpp`
  already lives).
* The fixture data under `src/test/fixtures/` needs **no** CMake registration.
  `MixxxTest::getTestDir()` resolves to the source `src/test` directory at test
  runtime (see `src/test/mixxxtest.h`), so the tests read the WAVs and JSON
  directly from the source tree.

---

## 5. How the beat F-measure helper works

Both `src/test/beatgridfixturetest.cpp` (C++) and `tools/check_fixtures.py`
(Python) implement the same algorithm and must stay in sync.

* Input: two lists of beat times in **seconds** — a candidate grid and the
  golden grid. Both are sorted first.
* A candidate beat is **correct** when it is within `toleranceSec = 0.070`
  (±70 ms, from `docs/beatgrid-audit.md` §8.2) of a golden beat.
* Matches are assigned **one-to-one and greedily** in time order (two cursors
  walk the sorted lists; whichever beat is earlier advances). This prevents a
  single candidate from matching two golden beats and vice versa.
* Precision = matched / (candidate count), Recall = matched / (golden count),
  F = 2·P·R / (P + R). An empty list yields 0.0.

Behavior verified by the tests and the Python self-check:

* Golden grid scored against itself → **F = 1.000000**.
* Golden grid uniformly shifted by +100 ms (beyond the 70 ms tolerance) →
  **F = 0.000000**, which fails the `< 0.5` threshold.
* Hand-verified helper cases: exact match → 1.0; missing one of three golden
  beats → precision 1.0, recall 2/3, F = 4/5 = 0.8; disjoint lists → 0.0;
  empty golden list → 0.0.

---

## 6. Assumptions and what still needs a build

These are documented rather than silently worked around:

1. **The real detector is not wired in.** `AnalyzerQueenMaryBeats` and the
   `AnalyzerBeats` pipeline are deliberately out of scope here: running them
   end to end requires the analyzer pipeline and a full build. The harness
   instead verifies (a) the fixture decodes with the expected properties and
   (b) the F-measure helper that any candidate grid is scored against. The
   `beatgridfixturetest.cpp` file ends with a comment describing exactly where
   the detector call would slot in: decode the click track, run the analyzer,
   collect the returned beat positions in seconds, and call
   `beatFMeasure(candidateBeats, m_goldenBeatsSec)`.
2. **8-channel WAV decoding.** The stem test relies on the generic audio source
   (`SoundSourceProxy` → FFmpeg/libav or libsndfile) decoding a plain
   8-channel WAV. `SoundSourceFFmpeg` decodes the stream's channel count as-is
   (`src/sources/soundsourceffmpeg.cpp` `initResampling`), so an 8-channel file
   decodes to 8 channels. This is an assumption until a build confirms it on
   all four targets.
3. **Little-endian packing.** The generators use `array('h')`, which is
   native-endian. Every supported CI target (Windows x64, Linux x64/arm64,
   macOS arm64/x86_64) is little-endian, matching the RIFF/WAVE container.
4. **No build was run for this workstream.** The CMake registration is
   intentionally out of scope; the C++ files have been written to match the
   existing test conventions (`gtest`, `MixxxTest`,
   `SoundSourceProviderRegistration`, `mixxx::AudioSource`) but have not been
   compiled. The Python generators and self-check **were** run and pass (below).

---

## 7. Captured self-check output

`py tools/check_fixtures.py`:

```text
Beatgrid fixture: C:\Users\Bradl\paramix\src\test\fixtures\beatgrid_128bpm.json
  bpm               = 128.0
  sample_rate       = 44100 Hz
  channels          = 1
  duration          = 8.0 s (352800 frames)
  beat period       = 0.468750 s (20671.875 frames)
  num beats         = 18
  beat 0..3 (s)     = 0.0000, 0.4688, 0.9375, 1.4062
  F-measure(correct grid)   = 1.000000
  F-measure(shifted +100ms) = 0.000000

Stem fixtures:
  stem_drums.wav   2 channels (ok)
  stem_bass.wav    2 channels (ok)
  stem_vocals.wav  2 channels (ok)
  stem_other.wav   2 channels (ok)
  stem_mix.wav     2 channels (ok)
  stem_8ch.wav     8 channels (ok)

PASS: fixtures are deterministic, consistent, and the golden-grid F-measure behaves as expected.
```
