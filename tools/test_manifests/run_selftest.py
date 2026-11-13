#!/usr/bin/env python3
"""Offline self-test for tools/paramix_model_fetch.py.

Runs entirely against local files (no network) and verifies the four behaviors
the workstream requires:

  A. clean download + verify succeeds (file:// source),
  B. a deliberately corrupted payload is refused with a non-zero exit and no
     finalized file,
  C. resume works (start, interrupt at the halfway point, then resume),
  D. --base-url overrides a manifest URL so an offline local copy is used.

Usage:
    py tools/test_manifests/run_selftest.py
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile

FETCH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "paramix_model_fetch.py"
)
PAYLOAD_SIZE = 1024 * 1024  # 1 MiB

MIT_TEXT = (
    "MIT License\n\n"
    "Copyright (c) 2026 Paramix contributors. "
    "Distributed under the MIT License."
)

PASSED = 0
FAILED = 0


def banner(title):
    print("\n" + "=" * 72)
    print("== " + title)
    print("=" * 72)


def check(name, condition, detail=""):
    global PASSED, FAILED
    if condition:
        print("PASS: " + name)
        PASSED += 1
    else:
        print("FAIL: " + name + ((" -- " + detail) if detail else ""))
        FAILED += 1


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(131072), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_url(path):
    return "file:///" + path.replace("\\", "/")


def make_model(model_id, url, sha, size):
    return {
        "id": model_id,
        "version": "1.0",
        "url": url,
        "sha256": sha,
        "size_bytes": size,
        "license": {"spdx": "MIT", "text": MIT_TEXT},
        "stem_layout": ["drums", "bass", "vocals", "other"],
        "sample_rate": 44100,
        "required_opset": 17,
    }


def write_manifest(path, models):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump({"format_version": 1, "models": models}, handle, indent=2)


def run_fetch(args, env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    cmd = [sys.executable, FETCH] + args
    suffix = "  (env: %s)" % json.dumps(env_extra) if env_extra else ""
    print("\n$ " + " ".join(cmd) + suffix)
    proc = subprocess.run(
        cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env
    )
    if proc.stdout:
        sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stdout.write(proc.stderr)
    print("(exit code: %d)" % proc.returncode)
    return proc


def main():
    workdir = tempfile.mkdtemp(prefix="paramix-model-fetch-selftest-")
    print("workdir: " + workdir)

    # ------------------------------------------------------------------ A
    banner("A. clean download + verify (file:// source)")
    fixture_a = os.path.join(workdir, "a")
    os.makedirs(fixture_a)
    payload_a = os.path.join(fixture_a, "payload.bin")
    with open(payload_a, "wb") as handle:
        handle.write(os.urandom(PAYLOAD_SIZE))
    hash_a = sha256_file(payload_a)
    manifest_a = os.path.join(fixture_a, "manifest.json")
    write_manifest(manifest_a, [make_model("test-model-a", file_url(payload_a), hash_a, PAYLOAD_SIZE)])

    out_a = os.path.join(workdir, "out-a")
    proc = run_fetch([manifest_a, "test-model-a", "--out", out_a])
    final_a = os.path.join(out_a, "payload.bin")
    check("exit code is 0", proc.returncode == 0, "got %d" % proc.returncode)
    check("final file exists", os.path.exists(final_a))
    check("final hash matches manifest", os.path.exists(final_a) and sha256_file(final_a) == hash_a)
    check("no .part leftover", not os.path.exists(final_a + ".part"))

    # ------------------------------------------------------------------ B
    banner("B. corrupted payload -> refusal, non-zero exit, no final file")
    fixture_b = os.path.join(workdir, "b")
    os.makedirs(fixture_b)
    corrupt = os.path.join(fixture_b, "corrupt.bin")
    data = bytearray(os.urandom(PAYLOAD_SIZE))
    data[0] ^= 0xFF  # flip one byte
    with open(corrupt, "wb") as handle:
        handle.write(data)
    # Point the manifest at the corrupted bytes but keep the ORIGINAL hash, so
    # the size matches while the digest must fail.
    manifest_b = os.path.join(fixture_b, "manifest.json")
    write_manifest(manifest_b, [make_model("test-model-b", file_url(corrupt), hash_a, PAYLOAD_SIZE)])

    out_b = os.path.join(workdir, "out-b")
    proc = run_fetch([manifest_b, "test-model-b", "--out", out_b])
    final_b = os.path.join(out_b, "corrupt.bin")
    check("exit code is non-zero", proc.returncode != 0, "got %d" % proc.returncode)
    check("exit code is 3 (integrity refusal)", proc.returncode == 3, "got %d" % proc.returncode)
    check("no finalized file", not os.path.exists(final_b))
    check("no .part leftover", not os.path.exists(final_b + ".part"))
    check("refusal message printed", "SHA-256 MISMATCH" in proc.stderr)

    # ------------------------------------------------------------------ C
    banner("C. resume: start, interrupt at halfway, then resume")
    fixture_c = os.path.join(workdir, "c")
    os.makedirs(fixture_c)
    payload_c = os.path.join(fixture_c, "resume.bin")
    with open(payload_c, "wb") as handle:
        handle.write(os.urandom(PAYLOAD_SIZE))
    hash_c = sha256_file(payload_c)
    manifest_c = os.path.join(fixture_c, "manifest.json")
    # A plain local path (no scheme) exercises the non-URL source path.
    write_manifest(manifest_c, [make_model("test-model-c", payload_c, hash_c, PAYLOAD_SIZE)])

    out_c = os.path.join(workdir, "out-c")
    half = PAYLOAD_SIZE // 2
    proc1 = run_fetch(
        [manifest_c, "test-model-c", "--out", out_c],
        env_extra={"PARAMIX_MODEL_FETCH_TEST_INTERRUPT_AT": str(half)},
    )
    part_c = os.path.join(out_c, "resume.bin.part")
    final_c = os.path.join(out_c, "resume.bin")
    check("interrupt exit code is 4", proc1.returncode == 4, "got %d" % proc1.returncode)
    check("partial .part file exists", os.path.exists(part_c))
    check(
        "partial .part is exactly half",
        os.path.exists(part_c) and os.path.getsize(part_c) == half,
        "size=%d" % (os.path.getsize(part_c) if os.path.exists(part_c) else -1),
    )
    check("no final file yet", not os.path.exists(final_c))

    proc2 = run_fetch([manifest_c, "test-model-c", "--out", out_c])
    check("resume exit code is 0", proc2.returncode == 0, "got %d" % proc2.returncode)
    check(
        "resume log mentions the resume offset",
        ("resuming at %d bytes" % half) in proc2.stderr,
    )
    check("final file exists after resume", os.path.exists(final_c))
    check("final hash matches", os.path.exists(final_c) and sha256_file(final_c) == hash_c)
    check("no .part leftover after resume", not os.path.exists(part_c))

    # ------------------------------------------------------------------ D
    banner("D. --base-url override (https URL redirected to a local directory)")
    fixture_d = os.path.join(workdir, "d")
    os.makedirs(fixture_d)
    local_remote = os.path.join(fixture_d, "remote.bin")
    with open(local_remote, "wb") as handle:
        handle.write(os.urandom(PAYLOAD_SIZE))
    hash_d = sha256_file(local_remote)
    manifest_d = os.path.join(fixture_d, "manifest.json")
    write_manifest(
        manifest_d,
        [
            make_model(
                "test-model-d",
                "https://github.com/christianstuart/paramix-models/releases/download/v1.0.0/remote.bin",
                hash_d,
                PAYLOAD_SIZE,
            )
        ],
    )

    out_d = os.path.join(workdir, "out-d")
    proc = run_fetch(
        [manifest_d, "test-model-d", "--out", out_d, "--base-url", file_url(fixture_d)]
    )
    final_d = os.path.join(out_d, "remote.bin")
    check("exit code is 0", proc.returncode == 0, "got %d" % proc.returncode)
    check("final file exists", os.path.exists(final_d))
    check("final hash matches", os.path.exists(final_d) and sha256_file(final_d) == hash_d)

    # ------------------------------------------------------------------ summary
    banner("summary")
    print("%d passed, %d failed" % (PASSED, FAILED))
    if FAILED:
        print("SELFTEST FAILED")
        return 1
    print("SELFTEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
