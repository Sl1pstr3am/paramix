# demucs-onnx (vendored)

Header-only C++ runtime for HTDemucs exported at the complex-as-channels
boundary.

- Upstream: <https://github.com/HRSadeghi/demucs-onnx>
- Vendored from commit: `757ab9face37c20edd2e08ac82410da82320763b`
- Imported files: `cpp/include/demucs/*.hpp`
- License: MIT (see `LICENSE`)

Only the header-only library is vendored. Model weights are **not** included.
The upstream runtime code is MIT, but the HTDemucs v3/v4 weights are Meta's and
are licensed **CC BY-NC 4.0** (non-commercial), so they must not be
redistributed with Paramix without a licensing decision.

Used by `src/stems/demucsonnxseparator.cpp` to provide the ONNX Runtime
session, STFT, chunking, track-level normalization, overlap-add and bag
blending.
