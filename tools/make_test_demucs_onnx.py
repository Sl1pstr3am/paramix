#!/usr/bin/env python3
"""Create a small deterministic Demucs-compatible ONNX test model."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--frames", default=1024, type=int)
    args = parser.parse_args()
    if args.frames <= 0:
        parser.error("--frames must be a positive integer")

    try:
        import onnx
        from onnx import helper, numpy_helper
    except ImportError as exc:
        raise SystemExit(
            "The 'onnx' package is required. Install it with: python -m pip install onnx"
        ) from exc

    frames = args.frames
    stft_frames = (frames + 1023) // 1024
    spec_shape = [1, 4, 4, 2048, stft_frames]
    wav_shape = [1, 4, 2, frames]

    import numpy as np

    initializers = [
        numpy_helper.from_array(
            np.asarray([1], dtype=np.int64), "unsqueeze_axis_1"
        ),
        numpy_helper.from_array(
            np.asarray(spec_shape, dtype=np.int64), "spec_expand_shape"
        ),
        numpy_helper.from_array(
            np.asarray(wav_shape, dtype=np.int64), "wav_expand_shape"
        ),
    ]
    nodes = [
        helper.make_node(
            "Unsqueeze",
            ["mag", "unsqueeze_axis_1"],
            ["mag_unsqueezed"],
            name="unsqueeze_mag",
        ),
        helper.make_node(
            "Expand",
            ["mag_unsqueezed", "spec_expand_shape"],
            ["spec"],
            name="expand_spec",
        ),
        helper.make_node(
            "Unsqueeze",
            ["mix", "unsqueeze_axis_1"],
            ["mix_unsqueezed"],
            name="unsqueeze_mix",
        ),
        helper.make_node(
            "Expand",
            ["mix_unsqueezed", "wav_expand_shape"],
            ["wav"],
            name="expand_wav",
        ),
    ]
    graph = helper.make_graph(
        nodes,
        "testdemucs",
        [
            helper.make_tensor_value_info(
                "mix", onnx.TensorProto.FLOAT, [1, 2, frames]
            ),
            helper.make_tensor_value_info(
                "mag", onnx.TensorProto.FLOAT, [1, 4, 2048, stft_frames]
            ),
        ],
        [
            helper.make_tensor_value_info(
                "spec", onnx.TensorProto.FLOAT, spec_shape
            ),
            helper.make_tensor_value_info(
                "wav", onnx.TensorProto.FLOAT, wav_shape
            ),
        ],
        initializer=initializers,
    )
    model = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", 17)],
        producer_name="make_test_demucs_onnx",
    )
    model.ir_version = 8
    onnx.checker.check_model(model)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    model_path = args.out_dir / "testdemucs.onnx"
    sidecar_path = args.out_dir / "testdemucs.json"
    onnx.save(model, model_path)
    metadata = {
        "name": "testdemucs",
        "sources": ["drums", "bass", "other", "vocals"],
        "samplerate": 44100,
        "audio_channels": 2,
        "segment_samples": frames,
        "nfft": 4096,
        "hop": 1024,
        "freq_bins": 2048,
        "frames": stft_frames,
    }
    sidecar_path.write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    print(model_path)
    print(sidecar_path)


if __name__ == "__main__":
    main()
