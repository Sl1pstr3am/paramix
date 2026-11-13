# ADR-002: Stem cache format and lifecycle

- **Status:** Proposed
- **Date:** 2026-11-12
- **Deciders:** Paramix maintainers
- **Supersedes:** none
- **Source material:** [`docs/paramix-stem-beatgrid-ui-plan.md`](../paramix-stem-beatgrid-ui-plan.md) §5.6, §6.3, §6.4
- **Related:** ADR-001 (inference runtime), ADR-003 (beatgrid format), ADR-005 (model distribution)

---

## Decision summary

1. Cache each separated track as **one 8-channel FLAC plus one JSON sidecar**, at the
   source track's sample rate and frame count, with an explicit channel map in the sidecar.
2. Store it under a **per-track generation directory**. Write to `tmp/`, rename into place,
   then commit the database row. The database row is the commit point; unreferenced
   directories are garbage collected.
3. Key entries by **source fingerprint + pipeline hash**, where the pipeline hash covers the
   model weights, separation parameters and cache format version. A model update therefore
   produces a new key instead of overwriting a valid entry.
4. Add a **`stem_cache` table** in schema revision 41. The migration is additive and
   reapply-safe, so `min_compatible` must remain low (`"3"`); otherwise older builds refuse
   to open the library.
5. The JSON sidecar is authoritative for the artifact; the database row is a rebuildable
   index. Losing the database does not require re-separating the library.
6. Playback uses the cache through a `SoundSourceStemCache` wrapper hooked into
   `SoundSourceProxy`. The existing 8-channel engine and analyzer paths do the rest; no
   engine rewrite.
7. The cache is **disposable**. It is never migrated across data-directory changes, never
   backed up, and may be deleted at any time without losing user data.

---

## 1. Context

### 1.1 What already exists

The playback half of stems is already in the tree. `SoundSourceSTEM` opens an NI
`.stem.mp4`, reads one premixed stereo stream plus four stereo stem streams, and
interleaves them as `1L1R2L2R3L3R4L4R` (`src/sources/soundsourcestem.cpp`, the loop in
`readSampleFramesClamped`). The engine consumes that layout: `ChannelCount::stem()` is 8
(`src/audio/types.h:99`), `kMaxSupportedStems` is 4 (`src/engine/engine.h:14`), and
`EngineDeck::processStem()` applies per-stem gain/mute/FX
(`src/engine/channels/enginedeck.cpp:130`).

The analyzers already understand 8-channel input. `AnalyzerWaveform` splits 8 channels into
four stereo stems (`src/analyzer/analyzerwaveform.cpp:66-68`, `:209-217`), and
`AnalyzerBeats` has a dedicated `ChannelCount::stem()` branch
(`src/analyzer/analyzerbeats.cpp:208-229`). The waveform pipeline stores per-stem data in
`WaveformData::stems[4]` (`src/waveform/waveform.h:31`), and the stem renderer already reads
it. This is why the cache does not need an engine rewrite: it only needs to provide the same
8-channel stream that `.stem.mp4` already provides.

`SoundSourceFLAC` has a generic multi-channel read path, not just mono/stereo
(`src/sources/soundsourceflac.cpp:499-501`), and initializes from the FLAC stream header's
channel count (`:518`). An 8-channel FLAC is therefore readable by the existing provider if
the encoder produces one.

### 1.2 What is missing

- **Generation.** Nothing separates an arbitrary track into stems.
- **Persistence.** `StemInfo` (labels and colors) exists only in memory
  (`src/track/track.h:353-363`, `:622-623`); it is re-imported from the `.stem.mp4` JSON
  atom on every parse (`src/track/track.cpp:1961-2021`). Generated stems have no such atom.
- **Lookup.** `SoundSourceProxy::openAudioSource()` opens the track's own file
  (`src/sources/soundsourceproxy.cpp:950-961`). There is no redirect to a generated stem
  artifact.
- **Index and GC.** The `track_analysis` table (`res/schema.xml:324-334`) is the precedent
  for derived per-track data, but there is no stem equivalent.

### 1.3 Constraints from the tree

- **The recording encoder cannot write the cache.** `Encoder::initEncoder()` takes only a
  sample rate (`src/encoder/encoder.h:31-32`), and `EncoderWave` hardcodes one or two channels
  from the recording settings (`src/encoder/encoderwave.cpp:74`, `:138-151`). A dedicated
  libsndfile writer is required; extending the recording UI to expose an 8-channel mode would
  leak an internal cache concern into a user-facing feature.
- **Schema downgrades are governed by `min_compatible`.** When a newer schema is opened by
  an older build, `SchemaManager` refuses only if the target version is below the recorded
  minimum (`src/database/schemamanager.cpp:124-133`, `:290-305`). A revision without
  `min_compatible` defaults to its own version (`:204-208`), which would lock out the
  previous build. Recent additive revisions use `min_compatible="3"`
  (`res/schema.xml:587-596`).
- **Derived data is deleted on purge, not on hide.** `TrackCollection::purgeTracks()` calls
  `m_analysisDao.deleteAnalyses(trackIds)` after the transaction
  (`src/library/trackcollection.cpp:404-409`). Hiding a track keeps its data.
- **There is no `fsync`/`QLockFile` helper in the tree.** Durability cannot rely on a
  framework primitive; the design must be recoverable from a crash by inspection, not by
  assuming a flush succeeded.

### 1.4 Decisions already made

- Models are download-only from the dedicated `paramix-models` repository, identified by a
  manifest with a SHA-256 and license.
- The data directory migrates to `~/.paramix`; the stem cache lives under it.
- `.stem.mp4` is the interoperability format, not the working cache.
- Separation is offline and background, not real-time.

---

## 2. Decision

### 2.1 Container and directory layout

One cache entry is a directory containing exactly two files:

```text
<root>/<track_id>/<generation>/stems.flac
<root>/<track_id>/<generation>/meta.json
```

- `<root>` defaults to `<settingsPath>/stems/` today and `<dataDir>/stems/` after the data
  directory migration. It is a setting (`[Stems],CacheDirectory`) so users can place it on a
  larger disk.
- `<track_id>` is the library track ID. It is stable across file moves and invalidates
  naturally when a track is purged and re-imported with a new ID.
- `<generation>` is a sortable unique token, for example
  `20261112T183500Z-<8 hex>`. It makes re-separation an atomic swap: a new generation is
  written and committed, and the previous generation becomes unreferenced garbage.
- `stems.flac` is a single 8-channel FLAC. `meta.json` is the self-describing sidecar in
  §2.4.
- Partial writes go to `<root>/tmp/<uuid>/` on the same filesystem. Rename into the final
  generation directory is atomic on POSIX and on Windows for a same-volume directory move.

A single 8-channel file is chosen over four stereo files because it is one seekable source,
one atomic unit, one checksum, and exactly the interleaved layout the engine already
consumes. Four files would require cross-file atomicity and four open handles per deck.

### 2.2 Audio format

| Property | Decision | Rationale |
|---|---|---|
| Codec | FLAC | Lossless, already linked through libsndfile and libFLAC, seekable, no new dependency |
| Channels | 8, interleaved as stem 0 L/R, stem 1 L/R, stem 2 L/R, stem 3 L/R | Matches `SoundSourceSTEM` and `EngineDeck::processStem()` |
| Sample rate | **The source track's sample rate**, not the model's 44.1 kHz | Beatgrid, cues and loop positions are in source frames; resampling the cache would misalign them |
| Frame count | **Exactly the source track's frame count**, frame 0 aligned | The cache must be a drop-in replacement for the original source; model latency is compensated by trimming/padding |
| Bit depth | **16-bit** default; 24-bit supported and recorded in metadata | Chosen for size; see the tradeoff below |
| Normalization | One scalar gain applied to all four stems so the reconstructed sum does not clip; stored in `meta.json` | Per-stem normalization would change the mix and break stem gain staging |
| Channel map | Explicit in `meta.json`; never inferred from order | NI files order stems drums/bass/other/vocals, but a future model may differ |

The canonical stem order for generated stems is **drums, bass, other, vocals** (the Demucs
and NI order), with the UI free to display them in any order. The sidecar's `stems[].channels`
array is authoritative; the FLAC channel order is an implementation detail of the writer.

**Bit-depth tradeoff:** at 44.1 kHz, 4 minutes, 8 channels, 16-bit FLAC is roughly
85–120 MB per track; 24-bit is roughly 130–180 MB. **16-bit is the default** because it
matches the plan's 50–120 MB estimate, keeps a library-sized cache within reach, and is
transparent for most playback chains. The cost is slightly more exposed quantization noise
when four stems are summed and heavily EQed, so 24-bit remains a supported value in the
sidecar and database; switching the default later does not change the format.

### 2.3 Cache identity and invalidation

Two hashes control lookup:

```text
source_fingerprint = SHA-256(canonical {
  track_id, file_size, duration_frames, sample_rate, channels
})

pipeline_key = SHA-256(canonical {
  cache_format_version,
  model_id, model_version, model_sha256,
  separation { window_frames, overlap, shifts, normalize }
})
```

- **`pipeline_key`** answers "which model and parameters produced this?" It deliberately
  excludes the runtime version and the GPU device: an ONNX Runtime or driver update must not
  invalidate a terabyte of cache. If a runtime change materially alters output, the model
  version or separation parameters must change.
- **`source_fingerprint`** answers "is this still the same audio?" It is intentionally cheap:
  no full-file hash on every lookup. A replaced file with identical size, duration, rate and
  channel count can produce a stale hit; `meta.json` also stores a full `source_sha256`
  computed during separation, and a "Verify cache" action re-hashes and compares.
- **Uniqueness:** one row per `(track_id, pipeline_key)`. Multiple models may have entries for
  the same track; the preferred model wins, and the fallback policy is defined in §2.7.
- **Re-separation** writes a new generation and updates the row. The old generation is
  unreferenced and collected; it is never deleted before the new row commits.

### 2.4 Metadata sidecar

`meta.json` is the authoritative description of the artifact. It is written once, never
edited in place. A representative schema:

```json
{
  "cache_format_version": 1,
  "created_at": "2026-11-12T18:35:00Z",
  "app": { "name": "Paramix", "version": "0.1.0" },
  "model": {
    "id": "htdemucs_ft",
    "version": "1.0.0",
    "sha256": "<weights hash>",
    "runtime": "onnxruntime",
    "runtime_version": "1.20.0",
    "device": "cpu"
  },
  "pipeline": {
    "window_frames": 344000,
    "overlap": 0.25,
    "shifts": 0,
    "normalize": "sum_peak",
    "params_sha256": "<hash>"
  },
  "source": {
    "track_id": 1234,
    "fingerprint": "<source_fingerprint>",
    "sha256": "<full source hash, for verify only>",
    "sample_rate": 44100,
    "channels": 2,
    "frame_count": 10584000,
    "file_size": 12345678
  },
  "audio": {
    "file": "stems.flac",
    "format": "flac",
    "sample_rate": 44100,
    "channels": 8,
    "bit_depth": 16,
    "frame_count": 10584000,
    "sha256": "<flac hash>"
  },
  "stems": [
    { "id": "drums", "label": "Drums", "color": "#E85D4A", "channels": [0, 1] },
    { "id": "bass", "label": "Bass", "color": "#7A5AF8", "channels": [2, 3] },
    { "id": "other", "label": "Other", "color": "#F2C94C", "channels": [4, 5] },
    { "id": "vocals", "label": "Vocals", "color": "#2BB673", "channels": [6, 7] }
  ]
}
```

Rules:

- Paths in the sidecar are **relative to the sidecar's directory** and are validated: no
  absolute paths, no `..`, no symlinks.
- `stems` must contain exactly four entries with unique `id` values and disjoint stereo
  channel pairs covering 0–7.
- Colors use a color-blind-safe palette and are always paired with a text label.
- Unknown fields are ignored on read; `cache_format_version` decides the parser.

### 2.5 Database index

Schema revision 41 adds a rebuildable index, not the artifact itself:

```sql
CREATE TABLE IF NOT EXISTS stem_cache (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  track_id INTEGER NOT NULL REFERENCES library(id),
  pipeline_key TEXT NOT NULL,
  source_fingerprint TEXT NOT NULL,
  source_sha256 TEXT,
  format_version INTEGER NOT NULL,
  model_id TEXT NOT NULL,
  model_version TEXT NOT NULL,
  model_sha256 TEXT NOT NULL,
  params_sha256 TEXT NOT NULL,
  relative_dir TEXT NOT NULL,
  audio_size INTEGER NOT NULL,
  audio_sha256 TEXT NOT NULL,
  meta_sha256 TEXT NOT NULL,
  sample_rate INTEGER NOT NULL,
  channel_count INTEGER NOT NULL,
  bit_depth INTEGER NOT NULL,
  frame_count INTEGER NOT NULL,
  stem_info TEXT NOT NULL,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
  last_used_at DATETIME DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(track_id, pipeline_key)
);
CREATE INDEX IF NOT EXISTS stem_cache_track_id_index ON stem_cache (track_id);
CREATE INDEX IF NOT EXISTS stem_cache_last_used_index ON stem_cache (last_used_at);
```

- The revision uses `CREATE TABLE IF NOT EXISTS` and `CREATE INDEX IF NOT EXISTS`, making it
  reapply-safe. It **must** carry `min_compatible="3"`, matching the recent additive
  revisions. Omitting the attribute defaults it to 41 and makes the previous build refuse to
  open the library (`src/database/schemamanager.cpp:204-208`).
- `stem_info` stores the sidecar's `stems` array as JSON, so the library can display labels
  and colors without opening files.
- The row is inserted **after** the generation directory is in place. A missing row means
  "not ready"; a directory without a row is garbage.
- `last_used_at` is updated by the deck-load path, not by every `SoundSource` open, and is
  throttled to at most one write per entry per hour. The analyzer and the caching reader must
  not turn playback into a database write storm.

Cleanup follows the `track_analysis` pattern: `TrackCollection::purgeTracks()` removes rows
for purged tracks in the same transaction, and file deletion is queued to the background GC.
Hiding a track keeps the cache. Orphan directories are removed at startup and after a
periodic sweep.

### 2.6 Atomic write and crash recovery

1. Create `<root>/tmp/<uuid>/`.
2. Write `stems.flac` with libsndfile (`SF_FORMAT_FLAC | SF_FORMAT_PCM_16` by default;
   `SF_FORMAT_PCM_24` when configured), 8 channels at the source rate. Encode from the
   interleaved stem buffers; apply the scalar headroom gain and clamp.
3. Write `meta.json` to a temporary file and rename it within the temp directory.
4. Flush and close both files. Compute the FLAC and sidecar SHA-256 values while writing or
   immediately after.
5. Rename `<root>/tmp/<uuid>/` to `<root>/<track_id>/<generation>/`.
6. Upsert the `stem_cache` row in a transaction. This is the commit point.
7. If any step fails, delete the temp directory and leave any previous generation and row
   untouched.

Recovery does not depend on `fsync` succeeding. At startup:

- Delete every directory under `<root>/tmp/`.
- For each generation directory, look for a matching `relative_dir` row. If none exists, delete
  the directory.
- For each row, verify that `relative_dir` exists and contains both files. If not, delete the
  row and let the next separation recreate it.
- Never delete a generation directory that a live row references, even if another generation
  for the same pipeline exists.

This tolerates a crash between any two steps: the worst case is an orphan directory or a row
whose files disappeared, both of which are recoverable by re-separation.

### 2.7 Lookup and playback integration

`SoundSourceStemCache` is a `SoundSource` wrapper that:

- Takes the cache entry's `stems.flac` path and the original track's identity.
- Delegates decoding to a `SoundSourceFLAC`.
- Reports the original track's sample rate and frame count (which the cache guarantees are
  identical, per §2.2).
- Returns 8 interleaved channels for normal deck requests.
- Mixes the four stereo stems down to stereo when `OpenParams` requests stereo, matching
  `SoundSourceSTEM`'s behavior for samplers and preview decks.
- Honors `OpenParams::stemMask()` so per-stem isolation works the same way it does for
  `.stem.mp4`.
- Returns `OpenResult::Failed` on any validation failure, so `SoundSourceProxy` falls back to
  the original file.

The hook goes in `SoundSourceProxy::openAudioSource()`
(`src/sources/soundsourceproxy.cpp:950-961`) before provider selection:

```cpp
if (m_pTrack) {
    if (auto pCacheSource = StemCache::openSource(*m_pTrack, params)) {
        // swap in the cache source and return it
    }
}
// existing openSoundSource(params) path
```

Validation before use: row exists, `source_fingerprint` matches, file exists, file size
matches, FLAC header reports 8 channels at the expected rate and frame count, and
`cache_format_version` is supported. A full SHA-256 is not computed on every load; it is used
by the "Verify cache" action and after a crash.

**Model selection and fallback.** Lookup first tries `[Stems],PreferredModel`. If that model
has no ready entry for the track, playback falls back to the best ready entry for any model,
rather than dropping to the original stereo file. "Best" means the highest-priority ready
model in the manifest (the quality tier order), then the most recently used. The active model
is shown in the deck and library badges, so a fallback is visible rather than silent. This
keeps stems available after a model change without forcing an immediate re-separation of the
whole library. A future setting may offer strict preferred-model behavior; it is not the
default.

**Analysis uses the cache too.** This is deliberate and matches `.stem.mp4`: `AnalyzerWaveform`
needs the 8 channels to produce `WaveformData::stems[4]`, and `AnalyzerBeats` already has a
stem branch. The cache hit must also trigger stem waveform analysis; the existing analysis
versioning decides what to regenerate. Analyzers that need stereo request it and receive the
mixed-down stream.

### 2.8 Garbage collection and disk policy

- **Default: no cap.** `[Stems],CacheLimitBytes` defaults to `0`, meaning unlimited.
  Separation continues until the free-space protection below stops it. The cache is expected
  to grow with the library, and the app must not silently delete stems the user waited for.
- **Optional cap.** If the user sets a limit, evict least-recently-used entries by
  `last_used_at` until the effective size — the directory size including the sidecar — is
  under the cap. Old-model entries are evicted before entries for the preferred model.
- **Pinning.** Entries for tracks loaded in a deck, queued for separation, or currently being
  written are pinned and skipped. This avoids deleting an open file on Windows and avoids
  evicting the track the user just loaded.
- **Free-space protection.** Without a cap, the app warns when free space falls below a
  threshold (for example 5 GB) and refuses to start a new separation when it is below a
  safety floor (for example 1 GB), with a clear error and a link to the cache settings. It
  does not delete entries automatically unless a cap is set.
- **Triggers.** Orphan sweeps run at startup and after a crash. When a cap is set, LRU
  eviction runs after each successful write and periodically. "Clear cache" and "Clear old
  models" are always available. Everything runs on a background thread, never the RT thread.
- **Preflight.** Before starting a separation, estimate the output size from the source frame
  count and refuse if free space is below the safety floor. If a cap is set and a single
  entry would exceed it, reject the job with an explanation rather than silently violating
  the cap.
- **Data-directory migration:** the cache is not migrated. On first run in the new layout,
  the old cache root is deleted or left for manual cleanup; tracks are re-separated lazily.
  This is safe because the cache is disposable.

### 2.9 Stem metadata and library display

- On a cache hit, `StemInfo` is populated from the row's `stem_info` JSON (or the sidecar when
  the row is being rebuilt). Generated stems no longer depend on an `.stem.mp4` atom.
- `.stem.mp4` files keep their existing atom import path; the cache is not consulted for
  them.
- The library gains a stem status badge derived from the presence of a `stem_cache` row and,
  later, the job state. It must not open `meta.json` per row.
- `StemInfo` persistence is therefore solved without a second `library` column: the cache row
  is the persistence for generated stems, and the atom remains the source for `.stem.mp4`.

### 2.10 Interoperability with `.stem.mp4`

The cache is **not** `.stem.mp4`:

- AAC is lossy, ALAC is larger and slower to write, and muxing four streams plus the NI JSON
  atom adds libmp4v2/FFmpeg work to the critical path.
- `.stem.mp4` remains the import format for existing files and the export format for Traktor
  interoperability.
- Export reads the 8-channel FLAC and muxes it into four ALAC or AAC streams plus the atom.
  It is a separate feature and does not change the cache layout.

### 2.11 Downgrade and forward compatibility

- Older builds ignore the `stem_cache` table and play the original files. They do not delete
  the table or the cache directory.
- Because revision 41 is additive and `min_compatible="3"`, an older build can open the
  library. A schema-40 build opening a revision-41 database must be part of the migration
  tests.
- A cache entry with an unknown `cache_format_version` is ignored, not deleted, until the GC
  decides it is unreferenced. A newer build can reuse it; an older build simply falls back.
- Deleting the database and rescanning does not require re-separation: the sidecar is
  authoritative, and a re-index can rebuild rows by scanning `meta.json`.

### 2.12 Security and integrity

- Validate `relative_dir` and every path in the sidecar: must be relative, must resolve under
  the cache root, must not contain `..`, and must not traverse symlinks.
- The cache root and entries are user-owned; do not run model files or executables from it.
  Model verification belongs to ADR-005.
- Store SHA-256 for the FLAC and sidecar. Verify fully on write, after a crash, and on the
  "Verify cache" action; verify size and header fields on every load.
- A corrupt entry is treated as a miss, logged, and left for GC. It never blocks playback.

---

## 3. Alternatives considered

### 3.1 `.stem.mp4` as the working cache

Rejected. It is the interoperability format, not the working format: AAC is lossy, ALAC is
larger and slower, and muxing adds complexity before the feature works. The existing reader
stays for imported files; export is a separate path.

### 3.2 Four stereo FLAC files

Rejected as the default. Four files need cross-file atomicity, four open handles per deck,
four checksums, and a directory scan to find a stem. A single 8-channel FLAC is one seekable
source and exactly the engine layout. If the 8-channel spike fails on a target, four stereo
FLACs become the documented fallback behind the same `StemCache` API.

### 3.3 Stems as SQLite BLOBs

Rejected. A 100–180 MB blob per track inflates the library database, defeats streaming and
seeking, slows backups, and makes corruption of one track a library-wide problem. Files plus
an index are the right split.

### 3.4 In-memory only, or real-time separation

Rejected for v1. Four stereo float stems for a 4-minute track are roughly 340 MB per deck;
real-time separation is 0.5–2 s latent at degraded quality and needs a GPU. Both are
research directions, not a cache.

### 3.5 Separate cache per deck

Rejected. Two decks loading the same track would duplicate hundreds of megabytes and repeat
the separation work. A shared per-track cache with runtime pinning is strictly better.

### 3.6 Store at the model's 44.1 kHz rate

Rejected. Beatgrid, cue and loop positions are expressed in the source track's frames.
Storing at 44.1 kHz would require resampling on playback and rescaling every position, which
is a new source of drift and bugs. Resample the model output back to the source rate, once,
during separation.

### 3.7 A new version string or a separate cache directory per version

Rejected for the same reason as ADR-003: version strings and directory names are not free.
The format carries `cache_format_version`; unknown versions are ignored, not destructive.

---

## 4. Consequences

**Positive**

- One file per track, atomic, seekable, checksummed, and self-describing.
- The existing 8-channel engine, analyzer and renderer paths are reused unchanged.
- The cache is disposable: no data migration, no backup burden, safe to delete.
- The database is a rebuildable index; losing it does not lose hours of separation work.
- Model updates do not destroy old entries; they coexist and are evicted by LRU when a cap is
  set, or by the explicit "Clear old models" action.
- `.stem.mp4` import remains untouched and export can reuse the same artifact.

**Negative**

- 85–120 MB per 4-minute track at 16-bit is still a real disk commitment, and the default is
  **no cap**, so the cache grows with the library until the free-space guard intervenes.
- A dedicated libsndfile writer is required; the recording encoder cannot be reused.
- Schema revision 41 plus a DAO, migration tests, GC and startup recovery are non-trivial.
- The source fingerprint is heuristic; a same-size replacement can produce a stale hit until
  verification.
- Multiple models multiply storage unless the user purges old entries; without a default cap,
  old-model entries are not evicted automatically.

**Risks and mitigations**

| Risk | Mitigation |
|---|---|
| 8-channel FLAC write/read fails on a target | Phase 0 spike across all four targets; documented four-stereo fallback behind the same API |
| `min_compatible` omitted, older builds locked out | Explicit `min_compatible="3"` and a schema-40 downgrade test |
| Crash leaves a partial entry | Temp directory, generation rename, row committed last, startup sweep |
| Re-separation deletes a live entry | New generation + row update; old generation unreferenced, then GC'd |
| GC deletes a file open in a deck (Windows) | Pin loaded tracks; skip pinned entries |
| Hashing 150 MB on every load | Size/header checks on load; full SHA-256 only on verify/repair |
| Disk exhaustion mid-job | Preflight free-space check; low-space warning; optional cap; pause with a clear error |
| Model update triggers a re-separation storm | New pipeline key; old entries remain; lazy re-separate; "Clear old models" and optional LRU evict them first |
| Same-size source replacement serves stale stems | Full `source_sha256` stored; "Verify cache" action; conservative invalidation |
| Multi-instance writers race | Unique DB constraint, generation directories, last-writer-wins; startup sweep |
| Path traversal or symlink attack | Relative-path validation, canonical-root check, no symlink traversal |

---

## 5. Implementation order

The first step is a hard gate; do not build the cache on an unproven container.

1. **Spike: 8-channel FLAC write/read on all four CI targets.** Write 8-channel FLAC at
   44.1/48/96 kHz, 16/24-bit with libsndfile; read it back through `SoundSourceFLAC` and
   `SoundSourceProxy`; verify channel count, rate, frame count and sample values within
   quantization tolerance. This is the Phase 0 "8-ch FLAC write/read" spike.
2. **Schema and DAO.** Revision 41 with `stem_cache` and indexes; `StemCacheDao` with
   lookup/upsert/delete/evict queries; migration test from a schema-40 database; downgrade
   test that opens revision 41 with a schema-40 target.
3. **Keys and sidecar.** Canonical JSON, `source_fingerprint`, `pipeline_key`, sidecar
   writer/parser, validation rules, path safety.
4. **Writer.** Dedicated libsndfile 8-channel FLAC writer, headroom gain, frame alignment,
   checksums, temp directory and generation rename.
5. **Reader and wrapper.** `SoundSourceStemCache`, stereo mixdown, `stemMask`, validation,
   `SoundSourceProxy` hook, fallback.
6. **Stem info and library status.** Populate `StemInfo` from the row/sidecar; add the
   status query for the library badge; keep `.stem.mp4` atom import unchanged.
7. **GC and recovery.** Startup sweep, LRU eviction when a cap is set, pinning, manual clear,
   low-space warning and free-space preflight.
8. **Tests.** Key stability across restarts; source invalidation; atomic write and crash
   recovery; GC order and pinning; schema downgrade; 8-channel round-trip; stereo mixdown;
   channel map validation; path traversal; corrupt entry fallback.
9. **Follow-on, separate changes:** separation queue and resume, model manager, export to
   `.stem.mp4`, library/UI status, settings page.

---

## 6. Acceptance

This ADR can be accepted before the separation model is implemented, because the cache is
independent of the model that fills it. Acceptance means:

- The container and sidecar schema in §2.1–§2.4 are frozen for cache format 1.
- The schema revision and `min_compatible` rule in §2.5 are requirements.
- The atomic-write and recovery rules in §2.6 are requirements.
- The 8-channel spike in §5.1 passes on all four targets before step 4 starts.
- The tests in §5.8 are part of "done", not follow-up work.

---

## 7. Product decisions

Recorded 2026-11-12:

1. **Bit depth: 16-bit.** Roughly 85–120 MB per 4-minute track, matching the plan's original
   estimate. 24-bit is a supported metadata value and can become a setting later without a
   format change.
2. **Default cache cap: none.** `[Stems],CacheLimitBytes` defaults to unlimited. The cache
   grows with the library; free-space warnings and a hard preflight floor replace automatic
   eviction. A user-set cap enables LRU eviction.
3. **Model fallback: best ready cache.** If the preferred model has no entry, playback uses
   the best available ready entry for the track and shows which model is active, rather than
   dropping to the original stereo file.

Two smaller calls, recorded for completeness: the canonical stem order is
drums/bass/other/vocals with an explicit channel map in the sidecar, and a single 8-channel
FLAC is the container, with four stereo FLACs as the documented fallback if the spike fails.
