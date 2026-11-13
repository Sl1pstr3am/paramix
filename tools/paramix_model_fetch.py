#!/usr/bin/env python3
"""paramix_model_fetch.py -- download and verify a Paramix model against a manifest.

Part of the Paramix project (GPL-2.0, based on Mixxx 2.7.0-alpha). Models are
download-only: they live in a dedicated `paramix-models` repository and are
never committed to this tree. This tool is the client-side scaffolding that:

  * reads a model manifest (JSON) and selects one model by id,
  * downloads the model from `url`/`urls`/`mirrors` (HTTP(S), `file://`, or a
    plain local path), resuming an interrupted transfer from a `.part` file,
  * verifies the byte size and SHA-256 digest against the manifest,
  * finalizes atomically (`.part` -> final) only after the digest matches,
  * refuses with a non-zero exit if the digest does not match.

It is dependency-free (Python 3 standard library only) so it can run anywhere,
including CI and offline machines.

Usage:
    py tools/paramix_model_fetch.py <manifest.json> <model-id>
    py tools/paramix_model_fetch.py <manifest.json> <model-id> --out DIR
    py tools/paramix_model_fetch.py <manifest.json> <model-id> --base-url file:///models
    py tools/paramix_model_fetch.py <manifest.json> <model-id> --fresh

Exit codes:
    0   success (downloaded and verified, or already present and verified)
    1   download/source failure (all sources unreachable)
    2   bad manifest, unknown model id, or invalid model entry
    3   integrity refusal: byte size or SHA-256 does not match the manifest
    4   simulated interruption (test seam, see below)

Test seam (for offline self-tests only):
    PARAMIX_MODEL_FETCH_TEST_INTERRUPT_AT=<bytes>
        After <bytes> have been written to the `.part` file, raise a simulated
        interruption. The partial file is left in place so a follow-up run can
        resume. Never set this in production.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.request
from urllib.parse import unquote, urlparse

USER_AGENT = "paramix-model-fetch/1.0 (+https://github.com/christianstuart/paramix)"
CHUNK_SIZE = 128 * 1024  # 128 KiB read/write granularity
RETRIES = 3
HTTP_TIMEOUT = 60.0
PROGRESS_STEP = 16 * 1024 * 1024  # log progress at most once per 16 MiB

REQUIRED_FIELDS = [
    "id",
    "version",
    "sha256",
    "size_bytes",
    "license",
    "stem_layout",
    "sample_rate",
    "required_opset",
]
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class FetchError(Exception):
    """A fatal download or source error."""


class ManifestError(Exception):
    """A malformed manifest or unknown model id."""


class _SimulatedInterrupt(Exception):
    """Raised by the test seam to simulate an interrupted download."""


def log(message=""):
    """Write a human-readable status line to stderr.

    Status goes to stderr so stdout stays clean for the one-line success
    summary (or the --json document).
    """
    print(message, file=sys.stderr)


def safe_remove(path):
    """Remove a file, ignoring a missing path."""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def sha256_file(path):
    """Return the lowercase hex SHA-256 digest of a file."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def has_scheme(url):
    """Return True if the string looks like a URL with a scheme."""
    return "://" in url


def is_file_url(url):
    return url.lower().startswith("file://")


def basename_from_url(url):
    """Return the last path component of a URL or local path, or None."""
    if has_scheme(url):
        name = os.path.basename(unquote(urlparse(url).path))
    else:
        name = os.path.basename(url)
    return name or None


def file_url_to_path(url):
    """Convert a `file://` URL to a local filesystem path.

    Handles the common Windows spellings (`file:///C:/...`, `file://C:/...`)
    and UNC paths (`file://server/share/...`) in addition to POSIX paths.
    """
    parsed = urlparse(url)
    path = unquote(parsed.path)
    netloc = parsed.netloc
    if netloc and netloc not in ("", "localhost"):
        if os.name == "nt" and re.match(r"^[A-Za-z]:$", netloc):
            # file://C:/... -> netloc is the drive letter
            path = netloc + path
        else:
            path = "//" + netloc + path
    if os.name == "nt":
        if path.startswith("//"):
            path = "\\\\" + path[2:].replace("/", "\\")
        elif re.match(r"^/[A-Za-z]:", path):
            path = path[1:]
        path = path.replace("/", "\\")
    return path


def maybe_interrupt(received):
    """Test seam: simulate an interruption after `received` bytes are written."""
    limit = os.environ.get("PARAMIX_MODEL_FETCH_TEST_INTERRUPT_AT")
    if limit is None:
        return
    try:
        limit = int(limit)
    except ValueError:
        return
    if received >= limit:
        raise _SimulatedInterrupt(received)


def _log_progress(received, total):
    if total and received >= total:
        log("  progress: %d / %d bytes (100%%)" % (received, total))


def _download_http(url, part_path, size_bytes, resume):
    """Download `url` into `part_path`, resuming from an existing prefix.

    Sends an HTTP `Range` header when resuming; if the server ignores it
    (returns a full 200 instead of 206) the transfer restarts from byte 0.
    """
    for attempt in range(RETRIES + 1):
        existing = os.path.getsize(part_path) if (resume and os.path.exists(part_path)) else 0
        if existing:
            log("resuming at %d bytes from %s" % (existing, url))
        else:
            log("downloading %s" % url)
        try:
            headers = {"User-Agent": USER_AGENT}
            if existing > 0:
                headers["Range"] = "bytes=%d-" % existing
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
                status = getattr(response, "status", 200)
                mode = "ab"
                offset = existing
                if existing > 0 and status != 206:
                    # The server ignored the Range header (or a redirect target
                    # stripped it): restart the transfer from byte 0.
                    log("server did not honor Range (HTTP %d); restarting" % status)
                    mode = "wb"
                    offset = 0
                received = offset
                next_report = ((offset // PROGRESS_STEP) + 1) * PROGRESS_STEP
                with open(part_path, mode) as handle:
                    while True:
                        chunk = response.read(CHUNK_SIZE)
                        if not chunk:
                            break
                        handle.write(chunk)
                        received += len(chunk)
                        maybe_interrupt(received)
                        if received >= next_report:
                            if size_bytes:
                                log("  progress: %d / %d bytes" % (received, size_bytes))
                            else:
                                log("  progress: %d bytes" % received)
                            next_report += PROGRESS_STEP
                _log_progress(received, size_bytes)
                return received
        except _SimulatedInterrupt:
            raise
        except Exception as exc:
            if attempt < RETRIES:
                log("retry %d/%d after error: %s" % (attempt + 1, RETRIES, exc))
                time.sleep(1.0 * (attempt + 1))
                continue
            raise FetchError(str(exc)) from exc
    raise FetchError("download failed after %d attempts" % (RETRIES + 1))


def _download_local(path, part_path, size_bytes, resume):
    """Copy a local file into `part_path`, appending from an existing prefix.

    Local sources have no HTTP Range, so resume is a seek-and-append: open the
    source, skip the bytes already present in `.part`, and copy the rest.
    """
    if not os.path.isfile(path):
        raise FetchError("local source not found: %s" % path)
    existing = os.path.getsize(part_path) if (resume and os.path.exists(part_path)) else 0
    source_size = os.path.getsize(path)
    if existing > source_size:
        # A stale `.part` longer than the source is not a valid prefix.
        existing = 0
    if existing:
        log("resuming at %d bytes from %s" % (existing, path))
    else:
        log("downloading %s" % path)
    received = existing
    with open(path, "rb") as source, open(part_path, "ab" if existing else "wb") as handle:
        source.seek(existing)
        next_report = ((received // PROGRESS_STEP) + 1) * PROGRESS_STEP
        while True:
            chunk = source.read(CHUNK_SIZE)
            if not chunk:
                break
            handle.write(chunk)
            received += len(chunk)
            maybe_interrupt(received)
            if received >= next_report:
                if size_bytes:
                    log("  progress: %d / %d bytes" % (received, size_bytes))
                else:
                    log("  progress: %d bytes" % received)
                next_report += PROGRESS_STEP
    _log_progress(received, size_bytes)
    return received


def _download_one(source, part_path, size_bytes, resume):
    """Download a single source into `part_path`, dispatching on the scheme."""
    if is_file_url(source):
        return _download_local(file_url_to_path(source), part_path, size_bytes, resume)
    if has_scheme(source):
        return _download_http(source, part_path, size_bytes, resume)
    return _download_local(source, part_path, size_bytes, resume)


def download(sources, part_path, size_bytes, resume):
    """Download from `sources` in order until one completes at the right size.

    A `.part` prefix is shared across sources (mirrors serve identical bytes),
    so a partial transfer from one source can be completed by the next. On a
    size mismatch the `.part` is discarded and the next source is tried.
    """
    failures = []
    for source in sources:
        try:
            _download_one(source, part_path, size_bytes, resume)
            got = os.path.getsize(part_path)
            if size_bytes is not None and got != size_bytes:
                safe_remove(part_path)
                raise FetchError(
                    "size mismatch for %s: expected %d bytes, got %d" % (source, size_bytes, got)
                )
            log("downloaded %d bytes" % got)
            return got
        except _SimulatedInterrupt:
            raise
        except Exception as exc:
            failures.append("%s: %s" % (source, exc))
            continue
    raise FetchError("all sources failed:\n  " + "\n  ".join(failures))


def models_in_manifest(manifest):
    """Return the list of model entries for any supported manifest shape.

    Supported shapes: an object with a `models` array, a single-model object
    (top-level `id`), or a bare array of model objects.
    """
    if isinstance(manifest, dict) and isinstance(manifest.get("models"), list):
        return manifest["models"]
    if isinstance(manifest, dict) and "id" in manifest:
        return [manifest]
    if isinstance(manifest, list):
        return manifest
    return None


def find_model(manifest, model_id):
    """Locate a model entry by id, or raise ManifestError."""
    models = models_in_manifest(manifest)
    if models is None:
        raise ManifestError(
            "manifest is neither a single model object nor an object with a 'models' array"
        )
    for model in models:
        if isinstance(model, dict) and model.get("id") == model_id:
            return model
    available = [m.get("id") if isinstance(m, dict) else "?" for m in models]
    raise ManifestError(
        "model id %r not found in manifest (available: %s)"
        % (model_id, ", ".join(map(str, available)) or "none")
    )


def validate_model(model):
    """Return a list of human-readable validation problems for a model entry."""
    errors = []
    if not isinstance(model, dict):
        return ["model entry is not an object"]
    for field in REQUIRED_FIELDS:
        if field not in model:
            errors.append("missing required field: %s" % field)
    if not model.get("url") and not model.get("urls"):
        errors.append("missing required field: 'url' or 'urls'")
    if "sha256" in model and not (
        isinstance(model["sha256"], str) and SHA256_RE.match(model["sha256"])
    ):
        errors.append("sha256 must be 64 hexadecimal characters")
    if "size_bytes" in model and not (
        isinstance(model["size_bytes"], int) and model["size_bytes"] >= 0
    ):
        errors.append("size_bytes must be a non-negative integer")
    if "license" in model:
        lic = model["license"]
        if not (isinstance(lic, dict) and lic.get("spdx") and lic.get("text")):
            errors.append("license must be an object with 'spdx' and 'text'")
    if "stem_layout" in model and not (
        isinstance(model["stem_layout"], list)
        and all(isinstance(s, str) for s in model["stem_layout"])
    ):
        errors.append("stem_layout must be an array of strings")
    if "sample_rate" in model and not (
        isinstance(model["sample_rate"], int) and model["sample_rate"] > 0
    ):
        errors.append("sample_rate must be a positive integer")
    if "required_opset" in model and not (
        isinstance(model["required_opset"], int) and model["required_opset"] >= 0
    ):
        errors.append("required_opset must be a non-negative integer")
    if "urls" in model and not isinstance(model["urls"], list):
        errors.append("urls must be an array of strings")
    if "mirrors" in model and not isinstance(model["mirrors"], list):
        errors.append("mirrors must be an array of strings")
    return errors


def model_sources(model):
    """Return the ordered source list: url/urls entries, then mirrors."""
    urls = model.get("urls")
    if not urls:
        urls = [model["url"]] if model.get("url") else []
    mirrors = model.get("mirrors") or []
    return list(urls) + list(mirrors)


def apply_base_url(base, url):
    """Override the base of a URL while preserving its filename."""
    name = basename_from_url(url) or ""
    if has_scheme(base):
        return base.rstrip("/") + "/" + name
    return os.path.join(base, name)


def build_sources(model, manifest_dir, base_url):
    """Resolve the model's source list into concrete URLs or local paths."""
    resolved = []
    for raw in model_sources(model):
        if base_url:
            resolved.append(apply_base_url(base_url, raw))
        elif has_scheme(raw) or os.path.isabs(raw):
            resolved.append(raw)
        else:
            # A relative path resolves against the manifest file's directory.
            resolved.append(os.path.join(manifest_dir, raw))
    return resolved


def derive_filename(sources, model):
    """Derive the final output filename from the first source's basename."""
    for source in sources:
        name = basename_from_url(source)
        if name:
            return name
    return "%s-%s" % (model["id"], model["version"])


def print_license_notice(model):
    """Print the size and license metadata before downloading."""
    lic = model.get("license") or {}
    spdx = lic.get("spdx", "unknown")
    text = (lic.get("text") or "").strip()
    log("")
    log("model:   %s %s" % (model["id"], model["version"]))
    log("size:    %d bytes" % model["size_bytes"])
    log("license: %s" % spdx)
    if text:
        log("license text:")
        for line in text.splitlines():
            log("  " + line)
    log("")


def print_summary(model, final_path, digest, as_json):
    """Print the success summary (human or JSON) to stdout."""
    if as_json:
        document = {
            "id": model["id"],
            "version": model["version"],
            "file": os.path.abspath(final_path),
            "sha256": digest,
            "size_bytes": os.path.getsize(final_path),
            "license": (model.get("license") or {}).get("spdx", "unknown"),
        }
        print(json.dumps(document, indent=2))
    else:
        log("file:    %s" % final_path)
        log("sha256:  %s" % digest)
        log("size:    %d bytes" % os.path.getsize(final_path))


def run(args):
    manifest_dir = os.path.dirname(os.path.abspath(args.manifest))
    with open(args.manifest, "rb") as handle:
        raw = handle.read()
    try:
        manifest = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ManifestError("could not parse manifest JSON: %s" % exc)

    model = find_model(manifest, args.model_id)
    problems = validate_model(model)
    if problems:
        raise ManifestError("invalid model entry %r:\n  " % args.model_id + "\n  ".join(problems))

    sha256 = model["sha256"].lower()
    size_bytes = model["size_bytes"]
    out_dir = args.out or "."
    os.makedirs(out_dir, exist_ok=True)

    sources = build_sources(model, manifest_dir, args.base_url)
    filename = derive_filename(sources, model)
    final_path = os.path.join(out_dir, filename)
    part_path = final_path + ".part"

    # Fast path: the final file already exists and verifies.
    if os.path.exists(final_path) and sha256_file(final_path) == sha256:
        log("already present and verified: %s" % final_path)
        print_summary(model, final_path, sha256, args.json)
        return 0

    print_license_notice(model)

    if args.fresh:
        safe_remove(part_path)

    resume = not args.fresh
    try:
        download(sources, part_path, size_bytes, resume)
    except _SimulatedInterrupt as exc:
        log("download interrupted at %d bytes (simulated test seam)" % exc.args[0])
        log("partial file kept for resume: %s" % part_path)
        return 4
    except FetchError as exc:
        log("download failed: %s" % exc)
        return 1

    actual_size = os.path.getsize(part_path)
    if actual_size != size_bytes:
        log("size mismatch: manifest says %d bytes, downloaded %d bytes" % (size_bytes, actual_size))
        safe_remove(part_path)
        return 3

    actual_digest = sha256_file(part_path)
    if actual_digest != sha256:
        log("")
        log("SHA-256 MISMATCH -- refusing to finalize the download.")
        log("expected: %s" % sha256)
        log("actual:   %s" % actual_digest)
        log("the .part file has been discarded; nothing was written to the final path.")
        safe_remove(part_path)
        return 3

    os.replace(part_path, final_path)
    log("verified and finalized: %s" % final_path)
    print_summary(model, final_path, actual_digest, args.json)
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="paramix_model_fetch.py",
        description="Download and verify a Paramix model against a manifest.",
    )
    parser.add_argument("manifest", help="path to the model manifest JSON file")
    parser.add_argument("model_id", help="model id to fetch from the manifest")
    parser.add_argument(
        "--out",
        default=".",
        help="output directory for the downloaded file (default: current directory)",
    )
    parser.add_argument(
        "--base-url",
        help="override the base of every manifest URL (the filename is preserved); "
        "accepts an http(s) URL, a file:// URL, or a local path",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="resume an interrupted download from the existing .part file "
        "(this is the default behavior)",
    )
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="discard any existing .part file and download from scratch",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print a machine-readable JSON summary to stdout on success",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        return run(args)
    except ManifestError as exc:
        log("manifest error: %s" % exc)
        return 2
    except KeyboardInterrupt:
        log("interrupted by user; the partial .part file was kept for resume")
        return 130


if __name__ == "__main__":
    sys.exit(main())
