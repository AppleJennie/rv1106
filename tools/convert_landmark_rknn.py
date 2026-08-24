#!/usr/bin/env python3
"""
Convert InsightFace 2d106det ONNX -> RKNN for RV1106.

The ONNX graph already contains (pixel - 127.5) / 128 normalization, so
rknn.config uses mean_values=[0,0,0], std_values=[1,1,1] and feeds raw
uint8/ float pixels.

Calibration:
    If --calib-dir is provided, the script expects Nx 192x192 face crop JPGs.
    Otherwise it creates a temporary synthetic calibration set of random
    192x192 images so the conversion pipeline can be exercised immediately.
    For production INT8 accuracy, replace with real face crops.

Target platform: rv1106 (rknn-toolkit2 v1.6.0 also accepts 'rv1103',
which shares the same NPU generation).
"""
import os
import sys
import argparse
import shutil
import tempfile
import numpy as np
import cv2

from rknn.api import RKNN


def make_synthetic_calibration(calib_dir: str, num: int = 100, seed: int = 42):
    """Create random 192x192 BGR images as a placeholder calibration set."""
    os.makedirs(calib_dir, exist_ok=True)
    rng = np.random.RandomState(seed)
    for i in range(num):
        # Random uint8 image; not representative of faces but sufficient
        # to exercise the INT8 quantization path.
        img = rng.randint(0, 256, (192, 192, 3), dtype=np.uint8)
        cv2.imwrite(os.path.join(calib_dir, f"calib_{i:04d}.jpg"), img)
    return calib_dir


def build_dataset_txt(calib_dir: str, txt_path: str):
    """RKNN dataset.txt: one image path per line (relative to working dir)."""
    base = os.path.dirname(txt_path) or "."
    with open(txt_path, "w") as f:
        for fn in sorted(os.listdir(calib_dir)):
            if fn.lower().endswith(('.jpg', '.jpeg', '.png', '.bmp')):
                rel = os.path.relpath(os.path.join(calib_dir, fn), base)
                f.write(rel + "\n")
    print(f"[INFO] dataset.txt written: {txt_path} ({sum(1 for _ in open(txt_path))} entries)")


def convert(onnx_path: str, rknn_path: str, calib_dir: str = None,
            target: str = "rv1106", do_quant: bool = True,
            verbose: bool = True):
    work_dir = os.path.dirname(os.path.abspath(rknn_path)) or "."
    tmpdir = None
    if not calib_dir:
        tmpdir = tempfile.mkdtemp(prefix="landmark_calib_", dir=work_dir)
        calib_dir = make_synthetic_calibration(tmpdir, num=100)
    dataset_txt = os.path.join(work_dir, "landmark_dataset.txt")
    build_dataset_txt(calib_dir, dataset_txt)

    rknn = RKNN(verbose=verbose)

    print("--> config model")
    # Keep raw pixels; normalization lives inside the ONNX graph.
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[1, 1, 1]],
        target_platform=target,
        # RV1106 has limited NPU memory; this can help if conversion fails.
        # For v1.6.0 see docs for exact API names; keep minimal first.
    )
    print("done")

    print(f"--> Loading ONNX model: {onnx_path}")
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        print("[ERR] load_onnx failed")
        return ret
    print("done")

    print(f"--> Building model (quantization={do_quant})")
    ret = rknn.build(do_quantization=do_quant, dataset=dataset_txt)
    if ret != 0:
        print("[ERR] build failed")
        return ret
    print("done")

    print(f"--> Exporting RKNN model: {rknn_path}")
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        print("[ERR] export_rknn failed")
        return ret
    print("done")

    rknn.release()

    if tmpdir and os.path.isdir(tmpdir):
        shutil.rmtree(tmpdir)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Convert 2d106det ONNX to RKNN")
    parser.add_argument("--onnx", default=None, help="path to fixed-shape ONNX")
    parser.add_argument("--rknn", default=None, help="output RKNN path")
    parser.add_argument("--calib-dir", default=None,
                        help="directory with 192x192 face crop calibration images")
    parser.add_argument("--target", default="rv1106",
                        help="RKNN target platform (default: rv1106)")
    parser.add_argument("--no-quant", action="store_true",
                        help="build FP16 model instead of INT8")
    args = parser.parse_args()

    root = os.path.dirname(os.path.abspath(__file__))
    onnx_path = args.onnx or os.path.join(root, "models", "landmark", "2d106det_1x3x192x192.onnx")
    rknn_path = args.rknn or os.path.join(root, "models", "landmark", "2d106det.rknn")

    if not os.path.exists(onnx_path):
        print(f"[ERR] ONNX not found: {onnx_path}")
        sys.exit(1)

    ret = convert(
        onnx_path,
        rknn_path,
        calib_dir=args.calib_dir,
        target=args.target,
        do_quant=not args.no_quant,
    )
    sys.exit(ret)


if __name__ == "__main__":
    main()
