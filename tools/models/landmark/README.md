# Landmark 模型（P2/P3 预备）

## 当前模型：InsightFace 2d106det

| 项目 | 说明 |
|------|------|
| 文件 | `2d106det.onnx` |
| 来源 | InsightFace `buffalo_l` 包，通过 ModelScope 镜像下载 |
| 输入 | `1x3x192x192`（已用 `prepare_landmark_onnx.py` 固定 batch） |
| 输出 | `1x212` -> reshape 为 `106x2`，对应 106 个 2D 关键点 |
| 预处理 | `(pixel - 127.5) / 128`，ONNX 图内已包含 Sub/Mul |
| 后处理 | `pred[:, 0:2] += 1; pred[:, 0:2] *= 96;` 得到 192x192 裁剪图上的像素坐标 |
| License | **仅限非商业研究使用**（InsightFace 预训练模型声明） |

## 文件清单

- `2d106det.onnx`：原始 ONNX（动态 batch）。
- `2d106det_1x3x192x192.onnx`：固定为 batch=1 的 ONNX。
- `2d106det.rknn`：待生成的 RKNN 模型（RV1106 INT8）。

## RKNN 转换

环境：`tools/.venv` 已安装 RKNN-Toolkit2 v1.6.0（跳过不需要的 torch/tensorflow，
ONNX-only 流程可正常工作）。

```bash
cd tools
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python prepare_landmark_onnx.py
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python convert_landmark_rknn.py
```

转换产物：
- `2d106det_1x3x192x192.onnx`（固定 batch）
- `2d106det.rknn`（RV1106 INT8，约 **1.4 MB**）
- 验证：`tools/verify_landmark_rknn.py` 在 RKNN 模拟器上与 ONNX 输出对比，
  最大像素误差 **< 1.0 px**。

### INT8 校准数据

当前脚本默认生成**随机 192x192 图片**作为占位校准集，仅用于跑通转换流程。
正式产品化前，必须用真实人脸 crop 替换：

```bash
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python convert_landmark_rknn.py \
    --calib-dir /path/to/face_crops_192x192
```

## 板端集成（已写骨架）

新增文件：
- `include/dms_face_landmark_106.h`
- `src/dms/dms_face_landmark_106.c`

开关：
```c
// include/common.h
#define DMS_ENABLE_LANDMARK_106    0   // 改成 1 即启用
```

调用链：
`dms_infer.c` → `dms_face_landmark_106_process(frame, face_result, &landmark_106_result)`

实现要点：
- 输入：`192x192 RGB`，`RKNN_TENSOR_UINT8`，NHWC。
- 裁剪策略：以 RetinaFace bbox 中心为中心，边长 `max(w,h)*1.5`，限制在原图范围内，再用 `dms_crop_and_resize` 缩放到 192x192。
- 输出反量化：支持 `FLOAT32` 和 `INT8`（RKNN 默认把 `fc1` 输出改成 `int8`）。
- 后处理：`pred += 1; pred *= 96` 得到 crop 内坐标，再按裁剪框比例映射回原图。

## 板端使用建议

1. 从 RetinaFace 输出的 bbox 取出一个**松散裁剪框**（建议 `max(w,h)*1.5` 边长、以 bbox 中心为 crop 中心）。
2. 仿射变换到 192x192，保持关键点可逆映射。
3. RKNN 输入使用 `RKNN_TENSOR_UINT8` 或 `FLOAT32`；由于图内已有归一化，可省去 mean/std。
4. 拿到 `1x212` 输出后，按 `后处理` 公式映射回 192x192 crop，再用逆仿射变换映射回原图。

## License 提醒

本目录下的 `2d106det.onnx` 来自 InsightFace 预训练模型，**禁止直接用于商业产品**。
若项目最终为商用，需替换为自训模型（如 PFLD）或取得商业授权。
