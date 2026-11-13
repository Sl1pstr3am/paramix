# Native stem file export

**Decision:** Traktor-compatible `.stem.mp4` **writing** is dropped. Paramix still
reads existing `.stem.mp4` files through `SoundSourceSTEM`, but generated stems are
written as a native Paramix set. There is no five-stream MP4 and no NI
`moov/udta/stem` atom.

## What is written

A set is a directory containing four stereo files and one sidecar:

```text
<StemFilesLocation>/<set-name>/
  drums.<ext>
  bass.<ext>
  other.<ext>
  vocals.<ext>
  stem-set.json
```

`<ext>` and the encoder are selected by the quality/time setting:

| Format | Extension | Encoder | Quality |
|---|---|---|---|
| `flac` (default) | `.flac` | libsndfile, 24-bit PCM | Lossless |
| `alac` | `.caf` | libsndfile, ALAC 24-bit in CAF | Lossless |
| `aac` | `.aac` | FDK AAC, ADTS, default 256 kbps | Lossy |

Lossless is bit-exact at 24-bit but larger and slower to write. AAC is much
smaller and faster, at the cost of quality.

## Settings

```ini
[Stems]
FileFormat=flac      ; flac | alac | aac
AacBitrate=256       ; kbps, clamped to 32..512
```

These are read by `mixxx::stems::readStemExportSettings()` and written by
`writeStemExportSettings()`. The Settings -> Stems UI will bind to the same keys.

## Sidecar

`stem-set.json` records the model, format, sample rate, frame count and the
four source files with their labels and colors:

```json
{
  "version": 1,
  "model": "htdemucs",
  "format": "flac",
  "lossless": true,
  "sample_rate": 44100,
  "frames": 123456,
  "sources": [
    { "name": "drums", "file": "drums.flac", "color": "#E85D4A" },
    { "name": "bass", "file": "bass.flac", "color": "#7A5AF8" },
    { "name": "other", "file": "other.flac", "color": "#F2C94C" },
    { "name": "vocals", "file": "vocals.flac", "color": "#2BB673" }
  ]
}
```

For AAC the sidecar also carries `"aac_bitrate_kbps"`.

## Atomicity

The writer creates a hidden temporary directory next to the destination, writes
all five files there, and renames the directory into place. A reader never sees
a partial set. An existing set with the same name is refused rather than
overwritten; the caller chooses a new name.

## Implementation

- `src/stems/stemfileformat.{h,cpp}` — format enum, setting keys, parse/name/
  extension/lossless helpers.
- `src/stems/stemfilewriter.{h,cpp}` — four-stem writer, FLAC/ALAC via
  libsndfile, AAC via the existing `EncoderFdkAac`, sidecar and atomic rename.
- `src/test/stemfilewritertest.cpp` — 10 tests: parsing, FLAC and ALAC
  round-trips, AAC smoke test, sidecar contents, existing-set refusal, invalid
  arguments and temporary-directory cleanup.

The internal playback cache remains the 8-channel FLAC from ADR-002; this export
is a separate, user-facing artifact.

## Not yet wired

- The schema revision 41 link and the library context action.
- Exposing the format keys in Settings -> Stems and the queue UI.
- Direct playback of an exported set (the 8-channel cache is still the playback
  path).
- Batch export and progress reporting.

## Licensing

FLAC and ALAC use libsndfile, which the tree already links. AAC uses the
dynamically loaded FDK AAC library that the recording feature already requires;
the AAC option is unavailable when that library cannot be loaded.
