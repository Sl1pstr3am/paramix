# paramix_model_fetch.py — offline self-test evidence

**Workstream:** W5 — Model hosting scaffolding (`paramix-models`)

This records a real, offline run of the model-fetch client scaffolding. The
harness is committed at `tools/test_manifests/run_selftest.py`; it never touches
the network — every source is a local file. Run it with:

```
py tools/test_manifests/run_selftest.py
```

The harness generates a 1 MiB random payload, computes its SHA-256, writes a
manifest, and drives `tools/paramix_model_fetch.py` through four cases:

| Case | Behavior proven |
|------|-----------------|
| **A** | Clean download + verify succeeds via a `file://` source (exit 0, final file present, digest matches, no `.part` leftover). |
| **B** | A deliberately corrupted payload is refused: exit 3, a clear `SHA-256 MISMATCH` message, and **no finalized file**. |
| **C** | Resume: the transfer is interrupted at exactly half, leaving a `.part` of 524288 bytes, then a follow-up run reports `resuming at 524288 bytes` and finalizes correctly. |
| **D** | `--base-url` overrides a manifest `https://` URL so an offline local directory is used (proves the mirror/CDN switch path). |

The temp directory name and the payload digests vary between runs (the payloads
are `os.urandom`); the exit codes, byte sizes, and PASS/FAIL verdicts are
deterministic.

---

## Captured terminal output

```
workdir: C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe

========================================================================
== A. clean download + verify (file:// source)
========================================================================

$ C:\Users\Bradl\AppData\Local\Python\pythoncore-3.14-64\python.exe C:\Users\Bradl\paramix\tools\test_manifests\..\paramix_model_fetch.py C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\a\manifest.json test-model-a --out C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-a

model:   test-model-a 1.0
size:    1048576 bytes
license: MIT
license text:
  MIT License

  Copyright (c) 2026 Paramix contributors. Distributed under the MIT License.

downloading C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\a\payload.bin
  progress: 1048576 / 1048576 bytes (100%)
downloaded 1048576 bytes
verified and finalized: C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-a\payload.bin
file:    C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-a\payload.bin
sha256:  cd70a8e7e3409dba015cc61af442bfbce40b302110f86372fbca09405de3c35b
size:    1048576 bytes
(exit code: 0)
PASS: exit code is 0
PASS: final file exists
PASS: final hash matches manifest
PASS: no .part leftover

========================================================================
== B. corrupted payload -> refusal, non-zero exit, no final file
========================================================================

$ C:\Users\Bradl\AppData\Local\Python\pythoncore-3.14-64\python.exe C:\Users\Bradl\paramix\tools\test_manifests\..\paramix_model_fetch.py C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\b\manifest.json test-model-b --out C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-b

model:   test-model-b 1.0
size:    1048576 bytes
license: MIT
license text:
  MIT License

  Copyright (c) 2026 Paramix contributors. Distributed under the MIT License.

downloading C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\b\corrupt.bin
  progress: 1048576 / 1048576 bytes (100%)
downloaded 1048576 bytes

SHA-256 MISMATCH -- refusing to finalize the download.
expected: cd70a8e7e3409dba015cc61af442bfbce40b302110f86372fbca09405de3c35b
actual:   11d3aa60938fc30021a952c8fcafc6623217f97a503f554525bb7d3e735e0335
the .part file has been discarded; nothing was written to the final path.
(exit code: 3)
PASS: exit code is non-zero
PASS: exit code is 3 (integrity refusal)
PASS: no finalized file
PASS: no .part leftover
PASS: refusal message printed

========================================================================
== C. resume: start, interrupt at halfway, then resume
========================================================================

$ C:\Users\Bradl\AppData\Local\Python\pythoncore-3.14-64\python.exe C:\Users\Bradl\paramix\tools\test_manifests\..\paramix_model_fetch.py C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\c\manifest.json test-model-c --out C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-c  (env: {"PARAMIX_MODEL_FETCH_TEST_INTERRUPT_AT": "524288"})

model:   test-model-c 1.0
size:    1048576 bytes
license: MIT
license text:
  MIT License

  Copyright (c) 2026 Paramix contributors. Distributed under the MIT License.

downloading C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\c\resume.bin
download interrupted at 524288 bytes (simulated test seam)
partial file kept for resume: C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-c\resume.bin.part
(exit code: 4)
PASS: interrupt exit code is 4
PASS: partial .part file exists
PASS: partial .part is exactly half
PASS: no final file yet

$ C:\Users\Bradl\AppData\Local\Python\pythoncore-3.14-64\python.exe C:\Users\Bradl\paramix\tools\test_manifests\..\paramix_model_fetch.py C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\c\manifest.json test-model-c --out C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-c

model:   test-model-c 1.0
size:    1048576 bytes
license: MIT
license text:
  MIT License

  Copyright (c) 2026 Paramix contributors. Distributed under the MIT License.

resuming at 524288 bytes from C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\c\resume.bin
  progress: 1048576 / 1048576 bytes (100%)
downloaded 1048576 bytes
verified and finalized: C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-c\resume.bin
file:    C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-c\resume.bin
sha256:  bc262be1c3a1d1af01888ef3ac0a799d24ec99e555494a588bee10bf62d984bd
size:    1048576 bytes
(exit code: 0)
PASS: resume exit code is 0
PASS: resume log mentions the resume offset
PASS: final file exists after resume
PASS: final hash matches
PASS: no .part leftover after resume

========================================================================
== D. --base-url override (https URL redirected to a local directory)
========================================================================

$ C:\Users\Bradl\AppData\Local\Python\pythoncore-3.14-64\python.exe C:\Users\Bradl\paramix\tools\test_manifests\..\paramix_model_fetch.py C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\d\manifest.json test-model-d --out C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-d --base-url file:///C:/Users/Bradl/AppData/Local/Temp/paramix-model-fetch-selftest-7yk5_3fe/d

model:   test-model-d 1.0
size:    1048576 bytes
license: MIT
license text:
  MIT License

  Copyright (c) 2026 Paramix contributors. Distributed under the MIT License.

downloading C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\d\remote.bin
  progress: 1048576 / 1048576 bytes (100%)
downloaded 1048576 bytes
verified and finalized: C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-d\remote.bin
file:    C:\Users\Bradl\AppData\Local\Temp\paramix-model-fetch-selftest-7yk5_3fe\out-d\remote.bin
sha256:  c91fbdd27cb3294fcdf62c83a02e17a13293eef3ae6933d77b0a899ea5b296c0
size:    1048576 bytes
(exit code: 0)
PASS: exit code is 0
PASS: final file exists
PASS: final hash matches

========================================================================
== summary
========================================================================
21 passed, 0 failed
SELFTEST PASSED
```

---

## Result

All four required behaviors are proven offline:

1. **Clean download + verify** succeeds (case A) with exit 0 and an
   atomic finalize.
2. **Corrupted payload** is refused (case B) with exit 3, a clear
   `SHA-256 MISMATCH` refusal, and **no finalized file** left behind.
3. **Resume** works (case C): an interruption at the halfway point leaves a
   `.part` of exactly 524288 bytes, and the follow-up run resumes from that
   offset and finalizes.
4. **Mismatch refusal** is guaranteed by the SHA-256 gate before the atomic
   rename, and a `file://` / plain-path source keeps the whole suite offline.
