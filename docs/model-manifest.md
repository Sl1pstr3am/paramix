# Paramix model manifest format

**Workstream:** W5 — Model hosting scaffolding (`paramix-models`)
**Status:** scaffolding spec · 2026-11-12

This document specifies the JSON manifest that describes a downloadable model for
the Paramix fork (GPL-2.0, based on Mixxx 2.7.0-alpha). Models are **download-only**
(decision 7 in `docs/paramix-stem-beatgrid-ui-plan.md` §5.9): no model weights are
committed to this repository or shipped in any installer. The manifest is the only
coupling between the app and the dedicated `paramix-models` repository.

The reference client is `tools/paramix_model_fetch.py`, which reads a manifest,
downloads and verifies a model, and refuses to finalize it unless the SHA-256
digest matches. See `tools/model_fetch_SELFTEST.md` for recorded evidence.

---

## 1. Where the manifest lives

The app pins a manifest URL (or bundles a signed copy) and never embeds model
weights. A single manifest file may describe many models; the client selects one
by `id`. Changing a mirror or moving to a CDN is a one-line manifest edit with no
app change (plan §5.9).

## 2. Top-level structure

The canonical form is a JSON object with a version marker and a `models` array:

```json
{
  "format_version": 1,
  "models": [ { "id": "...", "...": "..." } ]
}
```

| Top-level field | Type    | Required | Meaning                                            |
|-----------------|---------|----------|----------------------------------------------------|
| `format_version`| integer | yes      | Manifest schema version; currently `1`.            |
| `models`        | array   | yes      | One or more model entries (see §3).                |

For convenience the client also accepts a **single-model manifest** (a model
object at the top level, with an `id` field) or a bare **array of model objects**.
The multi-model `{ "format_version": 1, "models": [...] }` shape is the
recommended and canonical form.

## 3. Model entry schema

Each object in `models` describes exactly one downloadable model.

| Field            | Type                | Required            | Meaning                                                            |
|------------------|---------------------|---------------------|--------------------------------------------------------------------|
| `id`             | string              | **yes**             | Unique model id (e.g. `htdemucs_ft`).                              |
| `version`        | string              | **yes**             | Semantic version (e.g. `4.0.0`).                                   |
| `url`            | string              | one of `url`/`urls` | Primary download URL.                                              |
| `urls`           | array of strings    | one of `url`/`urls` | Ordered list of download URLs (tried in order).                    |
| `mirrors`        | array of strings    | no                  | Extra mirror URLs, tried after `url`/`urls`.                       |
| `sha256`         | string (64 hex)     | **yes**             | Lowercase hexadecimal SHA-256 of the exact downloaded bytes.       |
| `size_bytes`     | integer             | **yes**             | Exact byte size of the model file (used as a fast pre-check).      |
| `license`        | object              | **yes**             | License metadata (see §4).                                         |
| `license.spdx`   | string              | **yes**             | Short SPDX license id (e.g. `MIT`).                                |
| `license.text`   | string              | **yes**             | Full license text, retained verbatim (`LICENSE`/`NOTICE`).         |
| `stem_layout`    | array of strings    | **yes**             | Ordered output stems, e.g. `["drums","bass","vocals","other"]`.    |
| `sample_rate`    | integer             | **yes**             | Model input sample rate in Hz (e.g. `44100`).                      |
| `required_opset` | integer             | **yes**             | Minimum ONNX Runtime opset the model needs (e.g. `17`).            |

Rules enforced by `tools/paramix_model_fetch.py`:

- `sha256` must be exactly 64 hexadecimal characters.
- `size_bytes` must be a non-negative integer.
- `license` must be an object with non-empty `spdx` **and** `text`; a model with
  no license metadata is rejected before any download.
- `url` **or** `urls` must be present (either is sufficient).
- `stem_layout`, `sample_rate`, and `required_opset` are required and typed.

Unknown fields are ignored so the format can evolve without breaking old clients.

## 4. License metadata and the MIT-prompt requirement

Plan §5.9 requires the license to be shown to the user **before** the first
download. The manifest carries the license so the client never has to fetch it
separately:

```json
"license": {
  "spdx": "MIT",
  "text": "MIT License\n\nCopyright (c) ...\n..."
}
```

Client behavior (`tools/paramix_model_fetch.py`):

1. Before downloading, it prints the model id, version, byte size, SPDX id, and
   the full license text to standard error.
2. It **refuses** (exit code 2) any model whose `license` object is missing or
   malformed, so a model can never be installed without its license.

Hosting policy (plan §5.9): only **MIT** and **Apache-2.0** weights are hosted on
`paramix-models`. The default `htdemucs_ft` bag is MIT (retain its
`LICENSE`/`NOTICE`). Unlicensed community weights (RoFormer, MDX23C, UVR) are
never hosted; a user-supplied "Max" model stays local. The interactive
"show size + license, then accept" prompt is an app-layer concern; this
scaffolding exposes the license and refuses to proceed without it.

## 5. Example manifest

A realistic example (illustrative hash; see below for the runnable fixture):

```json
{
  "format_version": 1,
  "models": [
    {
      "id": "htdemucs_ft",
      "version": "4.0.0",
      "url": "https://github.com/christianstuart/paramix-models/releases/download/v4.0.0/htdemucs_ft.onnx",
      "mirrors": [
        "https://cdn.example.com/paramix-models/v4.0.0/htdemucs_ft.onnx"
      ],
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "size_bytes": 630000000,
      "license": {
        "spdx": "MIT",
        "text": "MIT License\n\nCopyright (c) ...\n..."
      },
      "stem_layout": ["drums", "bass", "vocals", "other"],
      "sample_rate": 44100,
      "required_opset": 17
    }
  ]
}
```

A tiny, fully runnable example is committed at
`tools/test_manifests/example-manifest.json`. It points at a small generated
fixture (`tools/test_manifests/example-payload.bin`) via a **relative path**, so
it runs offline:

```
py tools/paramix_model_fetch.py tools/test_manifests/example-manifest.json example-model
```

## 6. URL resolution and download sources

The client builds an ordered source list and tries each in turn until one
succeeds:

1. `urls` entries in order, or the single `url`;
2. `mirrors` entries in order.

A source may be:

- an `http://` or `https://` URL (downloaded with resume via HTTP `Range`);
- a `file://` URL (downloaded locally, so the tool is testable offline);
- a plain local filesystem path (no scheme), absolute or **relative**.

A relative path resolves against the **manifest file's directory**, which is how
the committed example stays runnable. `--base-url` overrides the base of every
manifest URL while preserving each URL's filename, so a CDN/mirror switch or an
offline directory can be substituted without editing the manifest.

## 7. Download, resume, and verification

`tools/paramix_model_fetch.py` downloads to a `.part` file and finalizes with an
atomic rename only after verification:

1. **Resume.** If a `.part` file exists, the transfer continues from its size.
   For HTTP this is a `Range: bytes=<offset>-` request (falling back to a full
   restart if the server ignores `Range`); for a local source it is a
   seek-and-append copy. A partial prefix is shared across mirrors (mirrors serve
   identical bytes).
2. **Size check.** The `.part` byte count must equal `size_bytes`.
3. **SHA-256.** The digest of the `.part` must equal `sha256`.
4. **Atomic finalize.** On a match the `.part` is renamed to the final filename;
   the rename is atomic on the local filesystem.
5. **Refusal.** On a size or digest mismatch the tool prints a clear refusal,
   discards the `.part`, writes nothing to the final path, and exits non-zero.

Exit codes: `0` success (or already-present-and-verified), `1` download/source
failure, `2` bad manifest or unknown id, `3` integrity refusal (size or SHA-256
mismatch), `4` simulated interruption (test seam only).

The final filename is derived from the first source URL's basename (falling back
to `<id>-<version>`). The app layer will install into
`<app-data>/models/<id>/<version>/` (plan §5.9) and add rich progress; this tool
prints coarse progress and a one-line (or `--json`) summary.

## 8. CLI

```
py tools/paramix_model_fetch.py <manifest.json> <model-id> [options]

  --out DIR       output directory (default: current directory)
  --base-url URL  override the base of every manifest URL (filename preserved);
                  accepts http(s), file://, or a local path
  --resume        resume from an existing .part file (this is the default)
  --fresh         discard any existing .part and download from scratch
  --json          print a machine-readable JSON summary on success
```
