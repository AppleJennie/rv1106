# TODO_HARDWARE - 需要真实硬件才能推进的事项

> 离线期间这些项**不阻塞**任何工作，也**不得伪造完成**。
> 回到开发板/车辆旁后按此清单逐项验收，结果标记 PENDING HARDWARE VALIDATION → PASS/FAIL。

## 已有清单（来自 00/08 交接文档）

- [ ] RV1106 ↔ MCU 真实 UART 联调（协议帧已单测通过，链路未实测）
- [ ] 蜂鸣器 GPIO 实测（MCU 侧 `dms_alarm_buzzer_hw_set()` 待 BSP 实现）
- [ ] 开机自启实机验证（`scripts/S99dms` + `dms_start.sh` 已写未部署）
- [ ] USB 供电稳定性整改（高负载掉电重启过 2 次：独立 5V / 优质线材 / 后置 USB）
- [ ] 软硬件看门狗实机验证
- [ ] 106 点模型真实人脸校准集重校准（`tools/convert_landmark_rknn.py --calib-dir`，100~200 张真实 crop）
- [ ] 微笑误判哈欠根治（内唇开度+嘴宽约束，方案已构思）
- [ ] 扭头姿态门槛（±25° 挂起判定）
- [ ] `camera_restart()` 后 RGA MB pool 重建
- [ ] ISP 座舱光照调优

## 本次离线总装新增

- [ ] STM32 真实 IMU 驱动接入（当前 HAL/stub）+ 安装方向实车标定
- [ ] 公交 CAN 真实 DBC 接入（当前 stub，禁止编造 CAN ID）
- [ ] GPS 实车数据接入 Route Risk
- [ ] Product Bridge 合入 RV1106 主程序（Integration V1 glue code，预计 <100 行）
- [ ] 真人疲劳阈值标定（Risk Manager 当前为工程默认值）
- [ ] 乘客舒适度阈值标定（Comfort Index 当前为内部工程指标）
- [ ] 服务器正式部署（端口待腾讯云控制台放行后确定；部署包在 `deploy/`，未启动）
