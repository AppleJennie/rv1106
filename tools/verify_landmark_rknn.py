#!/usr/bin/env python3
"""
Verify the converted 2d106det RKNN (via simulator) against the ONNX reference.

The ONNX model takes FLOAT32 raw pixels (0-255) and internally does
(pixel - 127.5)/128.  The RKNN build uses mean=[0,0,0] and std=[1,1,1],
so it expects UINT8 raw pixels.

Because INT8 quantization used synthetic random calibration data, expect
moderate numeric error; this script mainly validates graph/topology and
post-processing math.
"""
import os
import sys
import tempfile
import shutil
import numpy as np
import cv2
import onnxruntime as ort

from rknn.api import RKNN


def postprocess(pred: np.ndarray, input_size: int = 192):
    """InsightFace 2d106det postprocess: [-1,1] -> crop pixel coords."""
    pred = pred.reshape(-1, 2).copy()
    pred[:, 0:2] += 1.0
    pred[:, 0:2] *= (input_size // 2)
    return pred


def make_calib(calib_dir: str, num: int = 100, seed: int = 42):
    os.makedirs(calib_dir, exist_ok=True)
    rng = np.random.RandomState(seed)
    for i in range(num):
        img = rng.randint(0, 256, (192, 192, 3), dtype=np.uint8)
        cv2.imwrite(os.path.join(calib_dir, f"calib_{i:04d}.jpg"), img)


def main():
    root = os.path.dirname(os.path.abspath(__file__))
    onnx_path = os.path.join(root, "models", "landmark", "2d106det_1x3x192x192.onnx")

    if not os.path.exists(onnx_path):
        print(f"[ERR] ONNX not found: {onnx_path}")
        sys.exit(1)

    rng = np.random.RandomState(2024)
    img_u8 = rng.randint(0, 256, (192, 192, 3), dtype=np.uint8)

    # ONNX reference
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    blob = np.transpose(img_u8.astype(np.float32), (2, 0, 1))[np.newaxis, ...]
    onnx_out = sess.run(None, {sess.get_inputs()[0].name: blob})[0]
    onnx_kpts = postprocess(onnx_out)

    # RKNN build -> simulator inference (do not export, just verify)
    work_dir = os.path.join(root, "models", "landmark")
    tmpdir = tempfile.mkdtemp(prefix="verify_calib_", dir=work_dir)
    make_calib(tmpdir, num=100)
    dataset_txt = os.path.join(work_dir, "verify_dataset.txt")
    with open(dataset_txt, "w") as f:
        for fn in sorted(os.listdir(tmpdir)):
            f.write(os.path.relpath(os.path.join(tmpdir, fn), work_dir) + "\n")

    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]], target_platform="rv1106")
    rknn.load_onnx(model=onnx_path)
    rknn.build(do_quantization=True, dataset=dataset_txt)
    rknn.init_runtime()
    rknn_out = rknn.inference(inputs=[img_u8], data_format=["nhwc"])[0]
    rknn_kpts = postprocess(rknn_out)
    rknn.release()

    shutil.rmtree(tmpdir)

    print(f"ONNX output shape: {onnx_out.shape}")
    print(f"RKNN output shape: {rknn_out.shape}")
    print(f"ONNX keypoints first 5:\n{onnx_kpts[:5]}")
    print(f"RKNN keypoints first 5:\n{rknn_kpts[:5]}")
    diff = np.abs(onnx_kpts - rknn_kpts)
    print(f"Keypoint diff first 5:\n{diff[:5]}")
    print(f"Max absolute diff: {diff.max():.3f}")
    print(f"Mean absolute diff: {diff.mean():.3f}")

    if diff.max() < 20:
        print("[PASS] RKNN simulator and ONNX outputs are reasonably close for a sanity check.")
    else:
        print("[WARN] Large difference; likely due to synthetic calibration data. "
              "Recalibrate with real face crops before deployment.")


if __name__ == "__main__":
    main()
