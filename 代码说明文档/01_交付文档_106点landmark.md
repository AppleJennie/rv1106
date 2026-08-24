# 交付文档：106 点人脸 Landmark（InsightFace 2d106det）

> 编写日期：2026-08-23  
> 交付人：Kimi Code CLI  
> 说明：本文档汇总 P0 / P2 / P3 当前已完成的工作、已知风险，以及后续接手人“换模型继续”时需要改动的清单。

---

## 1. 交付概述

本项目目标板为 **LuckFox Pico RV1106（ARM 32-bit，uClibc）**，摄像头 **MIS5001 5MP RGB，1280x720 @ 15FPS JPEG**。

当前阶段：

| 阶段 | 内容 | 状态 |
|------|------|------|
| P0 | RetinaFace 替换 BlazeFace，绿框 + 5 点 landmark 显示，分项耗时统计，坐标诊断 | ✅ 已完成 |
| P0 | 板端实机验证（坐标精度、窗户/窗帘误检阈值） | ✅ 2026-08-23 上午验收通过（`acceptance/p0_*`） |
| P2/P3 | RKNN-Toolkit2 v1.6.0 转换环境 | PC 端可用 |
| P2/P3 | InsightFace 2d106det ONNX → RKNN（RV1106 INT8） | 已完成并校验 |
| P2/P3 | 板端 106 点 landmark C 推理骨架 | ✅ 已完成，板端实测通过（`acceptance/lm106_*`） |
| P4 | 疲劳检测（EAR/MAR/低头 + 状态机） | ✅ 2026-08-23 下午验收通过（`acceptance/fatigue_f13~f16`），见第 11 节 |

**核心交付原则**：P0 未验收前，106 点 landmark 模块通过宏开关 `DMS_ENABLE_LANDMARK_106` 完全隔离，不会影响 P0 的人脸检测验证。

---

## 2. 已完成工作清单

### 2.1 P0：RetinaFace 人脸检测（PC 端代码已ready）

- `src/dms/dms_retinaface.c`：RetinaFace RKNN 推理核心
  - 输入 640×640，16800 prior，INT8
  - 新增坐标反算诊断打印（`RETINAFACE_DEBUG_COORDS`，前 2 帧）
  - 新增分项耗时统计：`jpeg_decode / preprocess / rknn_run / postprocess / total`
- `src/dms/dms_face_detect.c`：JPEG 解码 + 调用 RetinaFace
- `src/dms/dms_visualize.c`：绿框 + 青色 5 点 landmark + HUD
- `src/dms/dms_ai_thread.c`：扩展 `[DMS PERF]` 日志
  - `raw_stream_fps / debug_stream_fps / ai_fps`
  - RetinaFace 各分项均值、可视化各分项均值
- 当前模型路径：`/mnt/sdcard/dms/models/retinaface.rknn`
- `HeadPose` 开关：`DMS_ENABLE_HEADPOSE = 0`

### 2.2 P2/P3：RKNN 转换环境

- 工具链位置：`tools/rknn-toolkit2/`
- 虚拟环境：`tools/.venv`（Python 3.8.10）
- 安装方式（已验证 ONNX-only 转换足够）：
  - 跳过了庞大的 `torch==1.10.1` / `tensorflow==2.8.0`
  - 仅安装核心依赖 + `rknn-toolkit2` 本地 whl `--no-deps`
- `distutils` 缺失问题：通过 `PYTHONPATH` 指向 `tools/.venv/lib/python3.8/site-packages/distutils_override` 临时绕过

### 2.3 P2/P3：106 点 Landmark 模型

| 项目 | 内容 |
|------|------|
| 原始 ONNX | `tools/models/landmark/2d106det.onnx` |
| 固定 batch ONNX | `tools/models/landmark/2d106det_1x3x192x192.onnx` |
| RKNN 产物 | `tools/models/landmark/2d106det.rknn` / `models/2d106det.rknn` |
| 输入 | `1×3×192×192`，RGB |
| 输出 | `1×212` → `106×2` |
| 模型大小 | 约 **1.4 MB**（INT8） |
| 预处理 | ONNX 图内已包含 `(pixel - 127.5) / 128`，RKNN 配置 `mean=[0,0,0], std=[1,1,1]` |
| 后处理 | `pred += 1; pred *= 96` 得到 192×192 crop 坐标 |
| 校验结果 | RKNN 模拟器 vs ONNX，最大像素误差 **< 1.0 px** |

### 2.4 板端 C 推理骨架

- 新增 `include/dms_face_landmark_106.h`
- 新增 `src/dms/dms_face_landmark_106.c`
- 已接入 `src/dms/dms_infer.c`
- 已加入 `CMakeLists.txt`
- 部署脚本已增加 `2d106det.rknn` 推送
- 宏开关默认关闭：`include/common.h` 中 `#define DMS_ENABLE_LANDMARK_106 0`
- 交叉编译通过（macro=0 和 macro=1 均通过）

---

## 3. 文件清单

### 3.1 新增文件

| 路径 | 说明 |
|------|------|
| `include/dms_face_landmark_106.h` | 106 点 landmark 接口头文件 |
| `src/dms/dms_face_landmark_106.c` | 板端 RKNN 推理实现（含 mock fallback） |
| `tools/prepare_landmark_onnx.py` | 固定 ONNX batch=1 |
| `tools/convert_landmark_rknn.py` | ONNX → RKNN 转换脚本 |
| `tools/verify_landmark_rknn.py` | RKNN 模拟器 vs ONNX 校验脚本 |
| `tools/models/landmark/2d106det.onnx` | 原始 ONNX |
| `tools/models/landmark/2d106det_1x3x192x192.onnx` | 固定 batch ONNX |
| `tools/models/landmark/2d106det.rknn` | RKNN 产物 |
| `tools/models/landmark/README.md` | 模型说明文档 |
| `models/2d106det.rknn` | 部署用 RKNN 产物副本 |
| `交付文档_106点landmark.md` | 本文档 |

### 3.2 修改文件

| 路径 | 修改点 |
|------|--------|
| `include/common.h` | 增加 `DMS_ENABLE_LANDMARK_106` 宏 |
| `include/dms_infer.h` | `dms_result_t` 在 macro=1 时增加 `landmark_106` 字段 |
| `src/dms/dms_infer.c` | 增加 106 点模块的 init/process/deinit 调用 |
| `CMakeLists.txt` | 增加 `src/dms/dms_face_landmark_106.c` |
| `scripts/deploy_dms.sh` | 增加 `2d106det.rknn` 推送 |

---

## 4. 如何复现模型转换与校验

```bash
cd /home/jennie/hhh/embed_complication/hand_capture_right/tools

# 1. 固定 batch
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python prepare_landmark_onnx.py

# 2. 转 RKNN（默认随机图校准，仅跑通流程）
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python convert_landmark_rknn.py

# 3. 模拟器校验
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python verify_landmark_rknn.py
```

如需真实校准数据：

```bash
PYTHONPATH=.venv/lib/python3.8/site-packages/distutils_override \
    .venv/bin/python convert_landmark_rknn.py \
    --calib-dir /path/to/face_crops_192x192
```

> 校准图要求：人脸 crop，尺寸 192×192，建议 100~200 张，JPG/PNG/BMP 均可。

---

## 5. 如何在板端启用/禁用 106 点 Landmark

### 5.1 启用

1. 修改 `include/common.h`：
   ```c
   #define DMS_ENABLE_LANDMARK_106    1
   ```
2. 重新交叉编译：
   ```bash
   cd build && cmake .. && make -j$(nproc)
   ```
3. 确保 SD 卡 `/mnt/sdcard/dms/models/` 下存在 `2d106det.rknn`：
   ```bash
   ./scripts/deploy_dms.sh
   ```

### 5.2 禁用

```c
#define DMS_ENABLE_LANDMARK_106    0
```

禁用后：
- 不会加载 `2d106det.rknn`
- 不会运行 106 点推理
- `dms_result_t` 不包含 `landmark_106` 字段
- 与 P0 验证状态完全一致

---

## 6. 后续若“换模型”需要改哪些地方

如果后续把 `2d106det` 换成 PFLD、PIPNet、自训模型等，请按下面清单修改。

### 6.1 Python 转换侧

| 文件 | 修改项 |
|------|--------|
| `tools/prepare_landmark_onnx.py` | `shape = [1, 3, H, W]` |
| `tools/convert_landmark_rknn.py` | `mean_values / std_values`（若新模型没有图内归一化） |
| `tools/verify_landmark_rknn.py` | 输入尺寸、后处理公式 |
| `tools/models/landmark/README.md` | 模型来源、输入输出、预处理、License |

### 6.2 板端 C 代码侧

| 文件 | 修改项 |
|------|--------|
| `include/dms_face_landmark_106.h` | `DMS_LANDMARK_106_NUM` → 新点数（如 98） |
| `src/dms/dms_face_landmark_106.c` | `LANDMARK_106_INPUT_W/H` → 新输入尺寸 |
|  | `LANDMARK_106_OUTPUT_LEN` = 点数 × 2 |
|  | `compute_loose_crop` 中的 `LANDMARK_106_CROP_SCALE`（默认 1.5）可按需调整 |
|  | `preprocess_crop` 中的通道顺序（RGB/BGR） |
|  | `read_output_floats` 输出类型/长度 |
|  | 后处理公式（当前 `pred += 1; pred *= 96` 是 2d106det 专用） |
| `src/dms/dms_infer.c` | `FACE_LANDMARK_106_MODEL_PATH` 模型文件名 |
| `scripts/deploy_dms.sh` | 推送的新模型文件名 |

### 6.3 可视化/调试侧（可选）

- `src/dms/dms_visualize.c`：增加 106 点（或新点数）绘制。
- `src/dms/dms_stream_server.c`：把关键点数组加入状态 JSON。
- `src/dms/dms_ai_thread.c`：把 landmark 耗时加入 `[DMS PERF]` 日志。

---

## 7. 已知问题与风险

### 7.1 P0 未完成

- **代码只在 PC 端编译通过**，未接 RV1106 实机。
- RetinaFace 绿框/5 点坐标精度、窗户/窗帘误检阈值仍需板端验证。
- **P0 不过，不要打开 `DMS_ENABLE_LANDMARK_106`。**

### 7.2 校准数据不真实

- 当前 `2d106det.rknn` 的 INT8 校准集是**随机 192×192 图片**。
- 模拟器校验误差 < 1 px 不代表板端真实人脸精度好。
- 正式部署前必须用人脸 crop 重新校准。

### 7.3 License 风险

- `2d106det.onnx` / `2d106det.rknn` 来自 **InsightFace 预训练模型**。
- **仅限非商业研究使用**。
- 若项目最终商用，必须替换为自训模型（如 PFLD）或取得商业授权。

### 7.4 映射方式待验证

- 当前 C 代码使用**简单 crop 缩放逆映射**（非 InsightFace 的仿射变换）。
- 对正脸足够；对侧脸、大姿态可能产生轻微偏移，需在板端实测后决定是否引入仿射变换。

### 7.5 RKNN 输出类型变化

- RKNN-Toolkit2 默认把 `fc1` 输出从 `float32` 改为 `int8`。
- 代码已做 `FLOAT32` / `INT8` 反量化兼容，但若换模型输出类型不同，需再检查。

---

## 8. 后续建议任务清单

- [ ] 接 RV1106 开发板，跑 P0 RetinaFace 验收。
- [ ] 收集真实人脸 crop，重新校准生成最终版 RKNN。
- [ ] 板端实测 106 点耗时、内存、关键点稳定性。
- [ ] 优化 5 点 / 106 点的坐标映射（必要时引入仿射变换）。
- [ ] 在 `dms_visualize.c` 中绘制 106 点。
- [ ] 在状态 JSON 中输出 106 点坐标。
- [ ] 评估 License 合规性，必要时迁移到自训 PFLD/PIPNet。

---

## 9. 附录：关键技术公式

### 9.1 输入预处理

2d106det ONNX 图内已做：

```
out = (pixel - 127.5) / 128
```

因此 RKNN 配置使用：

```python
rknn.config(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]], target_platform='rv1106')
```

### 9.2 输出后处理

RKNN 输出 `1×212`，reshape 为 `106×2`：

```python
pred[:, 0:2] += 1.0
pred[:, 0:2] *= 96.0   # 192 // 2
```

得到 192×192 crop 内的像素坐标。

### 9.3 crop → 原图映射

设 crop 在原图上的左上角为 `(crop_x, crop_y)`，宽高为 `(crop_w, crop_h)`：

```
orig_x = crop_x + px * crop_w / 192
orig_y = crop_y + py * crop_h / 192
```

### 9.4 裁剪框计算

以 RetinaFace bbox 中心为中心，边长：

```
side = max(face_w, face_h) * 1.5
```

并限制在原图范围内。

---

## 10. 联系方式/备注

- 所有 RKNN 转换脚本均已在 `tools/.venv` 中验证。
- 板端代码默认关闭，打开前请确认 P0 已通过。

---

## 11. 疲劳检测阶段更新（2026-08-23 下午）

### 11.1 本阶段新增

- `src/dms/dms_fatigue_features.c` / `include/dms_fatigue_features.h`：基于 106 点 landmark 的疲劳特征提取
  - EAR（双眼分别计算，左右按 x 坐标自动分配，防镜像写反）
  - MAR（嘴部开合度）
  - 低头几何代理（眼-鼻-下巴纵向比例）
  - 开机 2 秒自适应校准基线，阈值按基线比例生成
  - 状态机：`NORMAL / EYE_CLOSED / LONG_EYE_CLOSED / YAWN / HEAD_DOWN / NO_FACE`
- `status.json` 输出全部疲劳字段（ear/mar/基线/阈值/eye_closed/yawn/head_down 等）
- 可视化流叠加 106 点 + HUD
- 采集脚本模板：`acceptance/fatigue_f14_20260823_144503/record_f14.sh`（轮询 status.json 写 CSV + 状态切换自动截图）

### 11.2 关键 Bug 与修复

| 问题 | 根因 | 修复 |
|------|------|------|
| f13 采集 CSV 只有表头 | 采集脚本引用 status.json 中不存在的 `head_down_score/eye_closed/yawn` 字段，`None:.4f` 格式化抛异常，900 轮全部写失败 | stream server 补齐 3 个字段导出；采集脚本加默认值兜底 |
| f14 闭眼完全不触发 | AI 仅 2~3 FPS + EMA(α=0.45) 平滑，1~2 秒闭眼压不下 EMA；判定要求持续 800ms | ① EMA α 0.45→0.65（`common.h`）② 闭眼判定改用双眼 raw EAR 的 `fmax`（`dms_fatigue_features.c`），真闭眼双眼同时低才触发，同时防单眼关键点塌 0 误判 |

### 11.3 验收结论（f16 通过）

| 功能 | 结果 |
|------|------|
| EYE_CLOSED | ✅ 1~2 秒闭眼即可触发 |
| LONG_EYE_CLOSED | ✅（f16 连续 7 帧，截图确认真实闭眼） |
| YAWN | ✅（MAR 峰值 1.55，持续 1s 触发） |
| HEAD_DOWN | ✅（f14 验证） |
| 自适应基线 | ✅ 戴眼镜/不戴眼镜基线差异大（0.404 vs 0.263），按比例阈值均正常工作 |

### 11.4 遗留问题

- **USB 供电不稳**：f14 采集中途板子掉电重启一次（58°C、无 panic 日志、日志硬截断，判为 brownout）。建议换短粗 USB 线 / 后置 USB 口 / 独立 5V 供电。
- **单眼 EAR 间歇塌 0**：打哈欠眯眼时单边眼 EAR 偶发精确为 0（关键点退化）。闭眼判定已用 fmax 规避，但基线校准期若发生会拉低基线，需留意。
- **低头易丢脸**：大角度低头时 RetinaFace 检出率下降，HEAD_DOWN 判定依赖人脸在线，极端低头可能先进入 NO_FACE。
- **程序不随开机自启**：板子重启后需手动 `/root/start_hand_capture.sh`（或重新跑 deploy 脚本）。

---

## 12. V2-A：RGA 硬件预处理管线（2026-08-23 下午，已验收）

### 12.1 改造内容

AI 主路径从「VENC JPEG → stb 软件解码 → CPU resize」改为「VI NV12 DMABUF → RGA → NPU」：

```
VI NV12 DMABUF（Release 前）
   ├→ VENC → JPEG → /stream.mjpg（不动）
   ├→ RGA → 640×640 BGR（RetinaFace 输入，stretch）
   └→ RGA → 1280×720 RGB 全幅（106点 crop 源图）
```

- 新增 `src/dms/dms_rga_preprocess.c` / `include/dms_rga_preprocess.h`：RGA 预处理模块，RK MPI MB pool（CMA）三缓冲，latest-only，RGA 连续失败 30 次自锁回退软件路径
- `dms_retinaface.c` 新增 `dms_retinaface_process_prepared()`（跳过 CPU 预处理）
- `dms_infer.c` 新增 `dms_infer_process_prepared()`
- 宏开关 `DMS_HW_PREPROCESS`（默认 1，CMake `-DDMS_HW_PREPROCESS=OFF` 可完整回退软件路径）
- CSC 必须 full-range：RGA 默认 BT601 limited 与 stb/JPEG 的 full-range 不一致，会拉低 106 点精度；本版 librga (1.10.1) 的 `imsetColorSpace` 只有 C++ 符号，C 代码直接写 `rga_buffer_t.color_space_mode = IM_YUV_BT601_FULL_RANGE`
- 调试：`touch /mnt/sdcard/dms/DUMP_RGA` 可 dump RGA 源图 / 106 crop / retina 输入为 PPM 到 `/mnt/sdcard/dms/live/`
- 顺手修复：`dms_retinaface.c` / `dms_face_landmark_106.c` / `dms_face_landmark.c` deinit 中 `rknn_destroy_mem()` 后重复 `free()` 同一指针的 double-free（ALLOC_INSIDE 内存由 destroy 内部释放）

### 12.2 性能对比（板端实测）

| 指标 | V1 软件路径 | V2-A 硬件路径 |
|------|------------|--------------|
| 进程 CPU | 76.6% | **19.0%** |
| 系统 CPU | 92.4% | 38.3% |
| AI 帧率 | 3.7 FPS | **15 FPS（满帧）** |
| AI 单帧总耗时 | 262ms | 33ms |
| JPEG 软件解码 | 115ms | 0 |
| CPU resize | 116ms | 0（RGA 1.6ms+2.1ms） |
| AI 丢帧 | ~75% | 0 |

### 12.3 闭眼检测链路修复（同轮完成）

| 问题 | 修复 |
|------|------|
| EMA(α=0.45) 在低 AI 帧率下压不下闭眼信号 | α→0.65；闭眼判定改用双眼 raw EAR |
| 单眼关键点塌 0 误导判定 | 判定用 fmax(双眼) |
| 单眼关键点冲 1.0（fmax 被骗） | 新增 `DMS_EAR_SANE_MAX=0.60` 物理上限过滤：超过判该眼无效用另一眼，双眼无效保持 EMA 原值；过滤同样保护基线校正 |
| 阈值迟滞太窄（闭 0.17~0.21 vs 进入 0.180/恢复 0.208 打摆子） | EAR_CLOSE 0.65→0.72，EAR_RECOVER 0.75→0.85，MAR_YAWN 1.8→1.6，MAR_RECOVER 1.35→1.2 |

### 12.4 V2-A 验收结论

四种疲劳状态在硬件管线上全部实机触发：EYE_CLOSED / LONG_EYE_CLOSED / YAWN / HEAD_DOWN ✅（含戴眼镜场景）。

### 12.5 遗留（V2-B/C 候选）

- RGA 直写 RKNN input fd（消掉 take 侧 ~3.9MB memcpy，CPU 可再降几个点）
- 106 点 NV12 ROI 直裁（消掉 CPU crop）
- H.265 VENC + RTSP 替代 MJPEG、硬件 OSD 烧录绿框/HUD（消掉 debug 流的 stb 编解码）
- `camera_restart()` 后 MB pool 失效会自锁回退软件路径，如需保持加速要在重启后重建 RGA 模块
- 大角度低头仍可能先进入 NO_FACE（RetinaFace 对低头姿态检出率下降的固有问题）
- 祝换模型顺利。
