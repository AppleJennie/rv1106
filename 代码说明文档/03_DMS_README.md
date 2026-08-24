# DMS (Driver Monitoring System) Phase 3A

## 概述

DMS 模式使用 RV1106 NPU 运行真实 RKNN 推理，当前处于 **Phase 3A**：

- **视频链路**：摄像头 15FPS 采集 + MJPEG 推流（已与 AI 解耦）
- **AI 链路**：独立后台线程，只保留最新帧，跳帧不排队
- **人脸检测**：`retinaface.rknn`（RetinaFace，640×640 RGB/BGR，输出 bbox + 5 关键点）
- **头部姿态**：`face_landmark.rknn`（headpose）**当前默认禁用**，等待单独验证
- **疲劳判断**：Phase 3A 不输出疲劳状态

架构：

```text
capture thread:  camera_grab_jpeg → dms_stream_server_update_frame → submit latest frame
AI thread:       take latest frame → RetinaFace → update last_dms_result
```

AI 线程的任何失败或耗时都不会阻塞 MJPEG 推流。

## 文件结构

```
models/
  retinaface.rknn         # 从 luckfox_pico_retinaface_facenet Demo 复制
  face_landmark.rknn      # 从 SDK smart_door headpose.rknn 复制（当前禁用）

src/dms/
  dms_ai_thread.c         # AI 后台线程 + latest-frame 单槽缓冲
  dms_infer.c             # 推理编排
  dms_face_detect.c       # 人脸检测接口封装
  dms_retinaface.c        # RetinaFace 检测 + 5 关键点解码
  dms_face_landmark.c     # headpose 角度估计（当前默认禁用）
  dms_fatigue_logic.c     # 疲劳规则状态机（当前默认禁用）
  dms_image_utils.c       # JPEG 解码 / resize / grayscale
  dms_stream_server.c     # HTTP/MJPEG 推流

include/
  rknn_api.h              # RKNN Runtime API
  dms_ai_thread.h
  dms_image_utils.h
  dms_face_detect.h
  dms_face_landmark.h

3rdparty/stb_image/
  stb_image.h             # JPEG 解码
```

## 编译

```bash
cd build
rm -rf CMakeCache.txt CMakeFiles cmake_install.cmake Makefile
cmake ..
make -j$(nproc)
```

## 部署到 RV1106

一键脚本会自动推送二进制、模型、启动后台服务并设置 adb forward：

```bash
./scripts/deploy_dms.sh
```

或手动：

```bash
adb push build/hand_capture_right /root/hand_capture_right
adb shell chmod +x /root/hand_capture_right
adb shell "mkdir -p /mnt/sdcard/dms/models"
adb push models/retinaface.rknn        /mnt/sdcard/dms/models/retinaface.rknn
adb push models/face_landmark.rknn     /mnt/sdcard/dms/models/face_landmark.rknn
adb shell "mkdir -p /mnt/sdcard/dms/sessions"
adb shell /root/hand_capture_right
```

## 电脑端实时观看

部署脚本会自动执行 `adb forward tcp:8090 tcp:8090`，电脑直接：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop http://127.0.0.1:8090/stream.mjpg
```

或浏览器打开：

```text
http://127.0.0.1:8090/
```

可视化调试流（带人脸绿框 + HUD）：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop http://127.0.0.1:8090/stream_debug.mjpg
```

画面左上角叠加：STATUS / FACE score / PITCH / MAR / AI FPS。

状态接口：

```bash
curl http://127.0.0.1:8090/status.json
```

> 推流与 RKNN 推理已解耦：即使 AI 模型失败，ffplay 仍能正常看到实时摄像头画面。
> `/stream.mjpg` 保持原始画面不变；`/stream_debug.mjpg` 由 AI 线程异步生成，不会阻塞视频采集。

## 运行日志示例

Phase 3A 日志：

```text
[DMS PERF] camera_fps=15.0 stream_fps=15.0 ai_fps=2.1 camera_frames=96 ai_frames=13 ai_drop_latest=81
FACE_DETECT_OK face=1 score=0.779 bbox=0,0,1280,720
FACE_DETECT_OK no_face max_raw_score=0.435
DMS status=FACE face=1 score=0.78
DMS status=NO_FACE face=0 score=0.00
```

如果 BlazeFace RKNN 本身失败：

```text
FACE_DETECT_AI_ERROR ret=-1
```

## 已知限制

1. **headpose 已禁用**：`DMS_ENABLE_HEADPOSE=0`，因为当前 `face_landmark.rknn` 在 RV1106 上运行报 `invalid task index: 74`。需要先用最小测试程序 `test_headpose_rknn` 单独验证模型/Runtime 兼容性，再决定是否恢复。

2. **BlazeFace bbox 解码需要调优**：当前能检测到人脸（score ~0.78），但解码出的 bbox 经常覆盖整张图，说明 anchor decode 公式或 anchor 文件格式还需根据模型实际输出调整。已做图像边界 clamp，不会越界。

3. **Eye-closure 检测未启用**：BlazeFace 仅输出 5 个关键点，缺少眼睑轮廓，无法计算标准 EAR。当前 EAR 固定为 `0.30`。

4. **疲劳状态机未启用**：Phase 3A 只验证 BlazeFace，不输出 FATIGUE / HEAD_DOWN / EYE_CLOSED 等状态。

## 后续升级路径

- 获取/转换 68/106 点人脸关键点 RKNN 模型
- 在 `dms_face_landmark.c` 中用真实关键点计算 EAR/MAR
- 复用现有 `dms_fatigue_logic.c` 状态机即可触发 eye-closed 报警
