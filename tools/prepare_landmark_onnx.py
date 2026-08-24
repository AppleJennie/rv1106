#!/usr/bin/env python3
"""
Fix InsightFace 2d106det ONNX input shape to static 1x3x192x192
and write a normalized model (optional) for RKNN conversion.

Input preprocessing expected by 2d106det (inside the ONNX graph):
    (pixel - 127.5) * 0.0078125   i.e. (pixel - 127.5) / 128

The raw ONNX already contains the Sub/Mul nodes, so we keep the graph
as-is and only set the batch dimension to 1.
"""
import os
import sys
import onnx
from onnx import helper, TensorProto


def fix_input_shape(model_path: str, out_path: str, shape: list = None):
    if shape is None:
        shape = [1, 3, 192, 192]
    model = onnx.load(model_path)

    # Replace first input value_info with static shape
    old_vi = model.graph.input[0]
    new_vi = helper.make_tensor_value_info(
        old_vi.name,
        TensorProto.FLOAT,
        shape,
    )
    model.graph.input[0].CopyFrom(new_vi)

    # Run shape inference to propagate static batch
    try:
        model = onnx.shape_inference.infer_shapes(model)
    except Exception as e:
        print(f"[WARN] shape_inference failed: {e}")

    onnx.save(model, out_path)
    print(f"[OK] saved static-shape model to {out_path}")
    print(f"     input: {model.graph.input[0]}")
    print(f"     output: {model.graph.output[0]}")


if __name__ == "__main__":
    root = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(root, "models", "landmark", "2d106det.onnx")
    dst = os.path.join(root, "models", "landmark", "2d106det_1x3x192x192.onnx")
    if not os.path.exists(src):
        print(f"[ERR] source model not found: {src}")
        sys.exit(1)
    fix_input_shape(src, dst)
