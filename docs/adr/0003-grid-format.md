# ADR-003: Beatgrid format for bar and downbeat information

- **Status:** Proposed
- **Date:** 2026-11-12
- **Deciders:** Paramix maintainers
- **Supersedes:** none
- **Source material:** [`docs/beatgrid-audit.md`](../beatgrid-audit.md) §3
- **Related:** ADR-001 (inference runtime), ADR-002 (stem cache format)

---

## Decision summary

1. Store bar/meter information **inside the existing beatgrid protobuf**, not in a new
   database column.
2. Add it **additively** and **keep the existing version strings** `BeatGrid-2.0` and
   `BeatMap-1.0`. A new version string is destructive on downgrade.
3. Represent bar information as a **meter (`beats_per_bar`) plus a phase
   (`first_beat_in_bar`)**, relative to the grid's first beat. Do not store an absolute
   downbeat frame.
4. **Absence must be explicit.** A grid with no `bar` message means "bar information is
   unknown", never "4/4 starting on the first beat".
5. Cover the addition with **decoded-value round-trip tests** and a **downgrade
   simulation test**, because the existing tests only compare bytes to themselves and
   cannot detect a field the writer never emits.

---

## 1. Context

`Beats` currently models positions and tempo only. A `BeatMarker` is a tempo-section
border with two fields, `m_position` and `m_beatsTillNextMarker` (`src/track/beats.h:42-44`),
and the protobuf that serializes a grid has no attribute for a bar, a downbeat or a meter
(`src/proto/beats.proto`). The word "downbeat" appears in `src/` only in comments. There is
therefore nowhere to put the output of a downbeat detector even though one already exists
in the tree (`lib/qm-dsp/dsp/tempotracking/DownBeat.cpp`, compiled but unused).

The audit established three constraints that shape this decision:

1. **`getVersion()` is derived, not stored.** It returns `BeatGrid-2.0` for constant tempo
   and `BeatMap-1.0` otherwise (`src/track/beats.cpp:377-383`). Dispatch is hardcoded string
   comparison in `beats.cpp:262-270` and `beatfactory.cpp:78-96`.
2. **An unknown version is destructive.** `fromByteArray` returns `nullptr` for an unknown
   string (`beats.cpp:266-270`); `trackdao.cpp:1292-1296` then fabricates a frame-0 grid
   from the cached `bpm` column and marks the track dirty; the next save writes that
   placeholder over the original blob (`trackdao.cpp:1708-1710`). Shipping a new version
   string would make older builds **destroy grid phase on open**, not merely ignore it.
3. **The writers are selective.** `toBeatGridByteArray` emits only `first_beat.frame_position`
   and `bpm` (`beats.cpp:349-361`); `toBeatMapByteArray` emits only `frame_position` per beat
   (`beats.cpp:363-375`). Adding a field without extending the writer loses it on every save,
   and the current round-trip tests (`src/test/beatstest.cpp:395-404`, `:406-414`) compare
   `toByteArray()` with itself, so they cannot see the loss.

The current model also *implicitly assumes* the anchor is a downbeat. The class comment says
the last marker "is positioned at the first downbeat" (`beats.h:62-63`), and
`BeatUtils::makeConstBpm` re-anchors the grid with `fmod(regionStart, roundedBeatLength)`
while admitting in a comment that the anchor "should be the first proper downbeat"
(`src/track/beatutils.cpp:313-323`). The assumption is not representable, so it cannot be
corrected after detection or set by the user.

Finally, the database needs no change: grids are opaque blobs in the `library` table
(`res/schema.xml:286-290`), written by `trackdao.cpp:1708-1710`. Keeping the version strings
means **no schema revision and no migration**.

---

## 2. Decision

### 2.1 Wire format

Add one message and one field to each grid type in `src/proto/beats.proto`:

```proto
// Bar and meter information shared by BeatGrid and BeatMap.
message BarInfo {
  // Beats per bar, must be > 0 when present. The enclosing `bar` message is
  // what signals that bar information exists; absence of `bar` means the
  // meter and phase are unknown and must NOT be assumed to be 4/4.
  optional int32 beats_per_bar = 1;
  // Zero-based position of the grid's first beat within its bar.
  // 0 means the first beat is a downbeat. Must be in [0, beats_per_bar).
  // Defaults to 0 when absent, which is only meaningful because `bar` is present.
  optional int32 first_beat_in_bar = 2 [ default = 0 ];
}

message BeatGrid {
  optional Bpm bpm = 1;
  optional Beat first_beat = 2;
  optional BarInfo bar = 3;
}

message BeatMap {
  repeated Beat beat = 1;
  optional BarInfo bar = 2;
}
```

The `Beat` message is deliberately **not** changed. A single meter per track covers the
overwhelming majority of DJ material. If mid-track meter changes are ever needed, per-beat
fields can be added additively to `Beat` under the same version string; that extension does
not require reopening this decision.

`beats_per_bar` describes simple meters (3, 4, 5, 6, 7). Compound meters such as 6/8 need
a beat-unit concept that this model does not have; that is a documented limitation, not a
silent assumption. The field is a count of detected beats per bar, whatever the detector
calls a beat.

### 2.2 Meaning of the fields

`first_beat_in_bar` is the bar position of the **first beat of the grid**:

- For `BeatGrid`, that is `BeatGrid.first_beat` — the anchor stored as
  `m_lastMarkerPosition`. The anchor may be before frame 0 or in the middle of the track;
  it is not necessarily the track's first beat.
- For `BeatMap`, that is `beat[0]`, the first detected beat.

In code, **ordinal 0 is the beat at `cfirstmarker()`** — the anchor for a constant-tempo
grid and the first marker for a tempo map. Every other beat derives its bar position from its
ordinal relative to that beat:

```text
beat_in_bar(ordinal) = floorMod(first_beat_in_bar + ordinal, beats_per_bar)
is_downbeat(ordinal) = beats_per_bar > 0 && beat_in_bar(ordinal) == 0
```

`floorMod` is required, not `%`, because ordinals can be negative for beats before the
anchor of a constant-tempo grid. Two worked examples:

| `beats_per_bar` | `first_beat_in_bar` | Downbeats at ordinal |
|---|---|---|
| 4 | 0 | 0, 4, 8, … (and −4, −8) |
| 4 | 2 | 2, 6, 10, … (and −2, −6) |

The phase is stored rather than an absolute downbeat frame because phase is invariant under
the editing operations that move or rescale the whole grid. An absolute frame would have to
be rewritten by `tryTranslate` and would become wrong after `tryScale`.

### 2.3 In-memory representation

`Beats` gains two members with explicit unknown sentinels:

```cpp
// 0 means "bar information unknown". Valid values are >= 1.
int m_beatsPerBar = 0;
// Zero-based, meaningful only when m_beatsPerBar > 0.
int m_firstBeatInBar = 0;
```

Public accessors:

```cpp
bool hasBarInfo() const;          // m_beatsPerBar > 0
int beatsPerBar() const;          // 0 when unknown
int firstBeatInBar() const;       // 0 when unknown
int beatInBar(int beatOrdinal) const;   // floorMod, requires hasBarInfo()
bool isDownbeat(int beatOrdinal) const; // requires hasBarInfo()
```

Both constructors gain two trailing defaulted parameters,
`int beatsPerBar = 0, int firstBeatInBar = 0`, and `fromConstTempo`,
`fromBeatPositions` and `fromBeatMarkers` gain the same trailing parameters. Defaults keep
every existing call site compiling, including the many test call sites in
`src/test/enginesynctest.cpp`.

The sentinel `0` is chosen over `std::optional<int>` because the surrounding type is a
value type copied into every derived grid, and because `0` cannot be a valid meter.

### 2.4 Read and write rules

**Write.** `toBeatGridByteArray` and `toBeatMapByteArray` emit `bar` **only when
`hasBarInfo()` is true**. A grid that never had bar information must serialize exactly as it
does today, byte for byte, so existing libraries are untouched until a detector or the user
sets a downbeat.

**Read.** `fromBeatGridByteArray` and `fromBeatMapByteArray` accept `bar` and pass it into
the constructor. Validation is deliberately non-destructive:

| Condition | Behavior |
|---|---|
| `bar` absent | Load the grid with `hasBarInfo() == false` |
| `beats_per_bar` absent or `<= 0` | Ignore `bar`, log a warning, load the grid without bar info |
| `beats_per_bar` outside `1..32` | Ignore `bar`, log a warning, load the grid without bar info |
| `first_beat_in_bar` outside `[0, beats_per_bar)` | Ignore `bar`, log a warning, load the grid without bar info |
| `first_beat_in_bar` absent | Treat as `0` (only when `bar` and a valid meter are present) |

A malformed `bar` field must **never** cause the grid itself to be discarded. Losing bar
information is recoverable by re-analysis; discarding a good grid is not.

**Version.** `getVersion()` is unchanged. It continues to return `BeatGrid-2.0` or
`BeatMap-1.0` based on `hasConstantTempo()`, and a test asserts that a grid with bar
information still reports the old string.

### 2.5 Editing operations

All grid mutators preserve bar information:

- `tryTranslate` and `tryTranslateBeats` move every beat together; phase relative to the
  first beat is unchanged.
- `tryScale` changes the beat length but keeps the anchor; the meter and phase relative to
  the anchor remain valid for the new interpretation.
- `trySetBpm` flattens to a constant-tempo grid anchored at the first beat; the meter and the
  first beat's bar position are copied forward.
- `fromBeatPositions` must receive bar information as an argument, because it reconstructs
  markers from positions and cannot recover metadata that was never in the position list.

A future "set downbeat here" operation translates `first_beat_in_bar` so that the chosen
beat becomes ordinal `0 mod beats_per_bar`. It does not move any beat.

### 2.6 Downgrade behavior

This is the reason for the whole design. With an unchanged version string, an older build:

1. Parses the blob successfully into `BeatGrid` or `BeatMap`.
2. Preserves the unknown `bar` field in the parsed message's unknown-field set, but ignores
   it.
3. Rebuilds the in-memory grid from the **known** fields, so beat positions, BPM and tempo
   map are all preserved.
4. On save, constructs a fresh `BeatGrid` or `BeatMap` and writes only the known fields.
   `bar` is dropped.

Net effect: **the grid survives, the downbeat flag is lost.** That is acceptable and
recoverable. The alternative — a new version string — triggers the frame-0 placeholder path
and destroys grid phase permanently, which is not acceptable and cannot be fixed in builds
that are already released.

### 2.7 Required tests

Byte-for-byte round-trips are insufficient. The following are acceptance criteria for the
implementation, not optional extras:

1. **Decoded `BeatGrid` round-trip.** Build a constant-tempo grid with
   `beats_per_bar = 4`, `first_beat_in_bar = 2`. Serialize, deserialize, and assert
   `hasBarInfo()`, `beatsPerBar() == 4`, `firstBeatInBar() == 2`, and the expected
   `isDownbeat` values at ordinals −2, 2, 6, 10. Assert `getVersion() == "BeatGrid-2.0"`.
2. **Decoded `BeatMap` round-trip.** The same assertions for a variable-tempo grid, plus
   `getVersion() == "BeatMap-1.0"`.
3. **Absence round-trip.** A grid with no bar information deserializes to
   `hasBarInfo() == false`; the serialized bytes contain no `bar` field.
4. **Malformed input.** Hand-built blobs with `beats_per_bar = 0`, a meter above 32, and
   `first_beat_in_bar >= beats_per_bar` each load the grid, ignore bar info, and warn. The
   beat positions must match the same blob without `bar`.
5. **Downgrade simulation.** Parse a blob containing `bar`, call `DiscardUnknownFields()` on
   the protobuf message to emulate an older build, re-serialize using only the known fields,
   and re-parse. Assert the beat positions and BPM are identical and that bar info is gone.
   This test encodes the blast-radius claim so a future change cannot silently invalidate it.
6. **Mutator preservation.** `tryTranslate`, `tryScale`, `trySetBpm` return grids with the
   original `beatsPerBar()` and `firstBeatInBar()`.
7. **Downbeat math.** `beatInBar` and `isDownbeat` are correct for negative ordinals; this is
   where a plain `%` would fail.
8. **`operator==` regression.** `operator==` currently compares sample rates with `&&`
   instead of `==` (`beats.h:216-221`), which makes object-equality tests vacuous. Fix it as a
   prerequisite, and add a test where two grids differ only in bar information and are
   therefore unequal.

---

## 3. Alternatives considered

### 3.1 New version string (`BeatGrid-3.0` / `BeatMap-2.0`)

Rejected. It reads as the clean, honest option, but it is the only option that destroys data
in already-released builds. `fromByteArray` returns `nullptr` for an unknown string, after
which `trackdao` replaces the grid with a frame-0 placeholder and saves it. We cannot patch
builds that are already installed. Additive fields give the same semantic upgrade with a
safe failure mode.

### 3.2 Separate database column or sidecar table

Rejected. It requires a schema revision and a migration, splits the grid across two storage
locations with separate lifecycles, and makes the blob non-self-contained. It also does not
help third-party consumers that read the blob directly, and it duplicates the "unknown vs.
known" problem in SQL. The existing blob is the natural home for data that describes the
grid.

### 3.3 Per-beat bar attribute on `Beat`

Considered and deferred. It supports mid-track meter changes, but it also creates two
sources of truth (each beat's stored position versus the derived position), inflates
`BeatMap` by a few bytes per beat, and forces every reader to decide how to treat an
inconsistent sequence. A single track-level meter is sufficient for v1, and the field
number space on `Beat` remains available if per-beat data is needed later. That later change
is additive and does not need a new version string.

### 3.4 A boolean "is downbeat" per beat

Rejected. It records the answer but not the question: it cannot distinguish 4/4 from 3/4,
cannot render a "bar 1 vs. bar 2" distinction, and cannot be corrected when the meter is
wrong. It is also larger than two integers for a typical track.

### 3.5 Assume the first beat is the downbeat (status quo)

Rejected. The assumption is already embedded in `beats.h:62-63` and in
`BeatUtils::makeConstBpm`, and the audit shows it is wrong for the general case. Encoding an
assumption as a default is precisely how the current bug survives. Absence must mean
unknown, and the UI must offer a way to set the downbeat.

---

## 4. Consequences

**Positive**

- No schema revision, no migration, no new column.
- Older builds keep the grid; only bar information is lost on save.
- One representation works for constant-tempo and variable-tempo grids.
- The format can carry detected downbeats, user-set downbeats, and Rekordbox
  `beat_number` (1 = downbeat) without further format work.
- Rendering can accent downbeats from the existing beat iterator; no new data source.
- Engine Prime export can stop fabricating 4/4 (`engineprimeexportjob.cpp:120-167`).

**Negative**

- Older builds drop bar information on save. This is documented and accepted.
- One meter per track in v1; mid-track meter changes are deferred.
- Both `Beats` constructors and the three factory functions change signature. Defaulted
  parameters contain the churn, but the change touches every construction site in the
  analyzer, importers and tests.
- The `operator==` bug must be fixed before equality-based tests mean anything.

**Risks and mitigations**

| Risk | Mitigation |
|---|---|
| Writer forgets to emit `bar` | Decoded-value round-trip test (§2.7.1–2) fails |
| Reader assumes 4/4 when `bar` is absent | Explicit `hasBarInfo()` and absence test (§2.7.3) |
| Malformed bar info discards a good grid | Non-destructive validation table (§2.4) and test (§2.7.4) |
| A future contributor "cleans up" the version string | Downgrade simulation test (§2.7.5) encodes the constraint |
| Negative ordinals produce wrong downbeats | `floorMod` implementation and test (§2.7.7) |
| `BeatUtils` re-anchoring discards detected phase | Thread bar info through `BeatFactory`; do not re-derive it in `BeatUtils` |

---

## 5. Implementation order

Each step is blocked by the previous one. Steps 1–3 are the format change; 4–5 are the
consumers that make it visible.

1. **Prerequisite fix.** Correct `operator==` to compare sample rates with `==`
   (`src/track/beats.h:216-221`). Add the regression test.
2. **Protobuf.** Add `BarInfo` and the `bar` fields to `BeatGrid` and `BeatMap`
   (`src/proto/beats.proto`). No version-string change.
3. **Model.** Add the two `Beats` members, constructor parameters, accessors and
   `floorMod` helpers (`src/track/beats.h`). Extend `fromConstTempo`,
   `fromBeatPositions` and `fromBeatMarkers` with defaulted parameters.
4. **Serialization.** Extend both writers and both readers, including the validation table
   in §2.4 (`src/track/beats.cpp`).
5. **Factory.** Pass bar information through `BeatFactory::makePreferredBeats`
   (`src/track/beatfactory.cpp`) instead of letting `BeatUtils` re-anchor and discard it.
6. **Tests.** Implement §2.7 in `src/test/beatstest.cpp` (or a new
   `src/test/beatbarinfotest.cpp`).
7. **Follow-on work, separate changes:** analyzer output (`analyzerqueenmarybeats.cpp`,
   `analyzerplugin.h`), rendering accents (`waveformrenderbeat.cpp` and the allshader and
   QML variants), downbeat editing (`dlgtrackinfo.cpp`, `wtrackmenu.cpp`), and Rekordbox
   import (`rekordboxfeature.cpp:919-932`).

The format change in steps 2–4 does not depend on which detector populates the fields, so it
can land before the qm-dsp baseline spike. That ordering lets detection, rendering and
editing proceed in parallel once the format exists.

---

## 6. Open questions

1. **`first_beat_in_bar` indexing.** Decided zero-based in the format, because arithmetic is
   cleaner; the UI will display one-based beat numbers. Importers convert Rekordbox
   `beat_number` with `- 1`. Recorded here so the conversion is not reinvented per call site.
2. **Provenance.** Should bar information record whether it came from the analyzer or the
   user, so re-analysis does not overwrite a manual correction? Deferred. If needed, add a
   `Source source = 3` to `BarInfo` additively; do not add it unused, because `Beat.enabled`
   and `Beat.source` are already dead fields and a second dead field would be a regression in
   honesty.
3. **Mid-track meter changes.** Deferred to a per-beat additive extension. Not required for
   v1 and not worth the consistency burden now.
4. **Compound meters.** `beats_per_bar` counts detected beats, not notated beat units. A
   later beat-unit field would be additive.

---

## 7. Acceptance

This ADR can be accepted before the qm-dsp spike, because the format is required regardless
of the detector. Acceptance means:

- The wire format in §2.1 is frozen for v1.
- The validation and downgrade rules in §2.4 and §2.6 are requirements, not suggestions.
- The tests in §2.7 are part of "done", not follow-up work.
