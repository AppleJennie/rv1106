# 交接文档 09：GitHub 备份记录 + 离线总装（Agent C）启动

> 编写日期：2026-08-25
> 前置文档：`00_交接文档.md`（视觉链 V2-A）、`08_交接文档_DMS_ProductEngineering_MCU.md`（产品化+MCU 模块）
> 本文档内容：① GitHub 备份的事实记录与复现方法 ② 下一步离线总装任务清单与边界

---

## 1. GitHub 备份：事实记录

| 项 | 值 |
|----|----|
| 远端仓库 | `git@github.com:AppleJennie/rv1106.git` |
| 分支 | `main` |
| 首个提交 | `77113d8`（根提交，407 个文件，约 40MB） |
| 提交身份 | `AppleJennie <applejennie@users.noreply.github.com>` |
| 推送日期 | 2026-08-25 |

包含：全部源码（`src/ include/ mcu/ beep_warn/ tests/`）、模型（`models/`，约 4.2MB）、脚本、验收记录（`acceptance/`，约 7.7MB）、文档、转换工具脚本、ONNX 中间产物。

不包含（见 `.gitignore`，均可重建/重新下载）：

- `tools/.venv`、`tools/rknn-toolkit2`（约 2.8GB，pip 重装）
- `opencv-mobile-4.10.0-luckfox-pico/`（60MB，官方 release 下载）
- `build*/` 构建产物、编译出的可执行文件
- `tools/pip.whl`、`tools/distutils.deb`、`tools/distutils_extract/`

### 1.1 脱敏记录

- `scripts/S41wifi_ap`、`scripts/hostapd.conf` 中的热点密码已替换为占位符 `CHANGE_ME_WIFI_PSK`。
  **部署到板子前必须改回真实密码**，否则热点起不来。
- 全库扫描过 API key / token / password / 私钥，除上述 WiFi 密码外无真实密钥。

### 1.2 License 提醒

`models/2d106det.rknn`（及 `tools/models/landmark/2d106det*.onnx`）来自 InsightFace，**仅限非商业研究**。仓库建议保持 **private**，避免模型二次分发风险；商用前换自训模型或取得授权（同 00 文档第 6 节）。

---

## 2. 本机网络环境与 SSH 配置（重要，换机器要重做）

本机访问 GitHub 是"半通"状态，事实如下：

- **DNS 被污染**：`github.com` 解析到 `::1`（本机），直接 keyscan 到的是假服务，**绝不能信**。
- **真实 GitHub IP 的 22 端口可达**（本次实测 `140.82.112.3` / `140.82.113.3` / `140.82.114.3` / `140.82.116.3` 均通）。
- 已写入 `~/.ssh/config`：

```sshconfig
Host github.com
    HostName 140.82.112.3
    IdentityFile ~/.ssh/id_ed25519
    IdentitiesOnly yes
    CheckHostIP no
```

- 主机密钥已核对 GitHub 官方 ed25519 指纹并写入 `~/.ssh/known_hosts`：

```text
SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU   ← 官方指纹，必须一致
```

**安全规则**：以后若连接时提示 host key 变化，先核对上面这个指纹，不一致就不要连。

- 本机密钥对：`~/.ssh/id_ed25519`（私钥，勿外传）/ `~/.ssh/id_ed25519.pub`，公钥指纹 `SHA256:YsPKmetUwZ8124LErhYY3JbYC8GVtssTHTIubotdccQ`，已挂到 GitHub 账号 AppleJennie。
- 若 140.82.112.3 以后不通了：用 `ssh-keyscan -t ed25519 <候选IP> | ssh-keygen -lf -` 找指纹匹配官方值的可达 IP，改 `~/.ssh/config` 的 HostName 即可。

### 2.1 以后每次推送的命令

```bash
cd /home/jennie/hhh/embed_complication/hand_capture_right
git add -A
git -c user.name="AppleJennie" -c user.email="applejennie@users.noreply.github.com" commit -m "提交说明"
git ls-remote git@github.com:AppleJennie/rv1106.git   # 先看远端状态
git push origin main
```

规则：**禁止 force push**；远端若有他人提交先 fetch 合并；推送前重新扫一遍密钥（尤其新加的配置文件）。

---

## 3. 下一步：离线总装（Agent C）任务清单

原则：**所有不依赖实机硬件的工作连续做完；硬件项记录到 `TODO_HARDWARE.md`，不阻塞、不伪造完成。**

产品价值观红线（所有代码必须遵守）：

- AI 是安全提示系统，**不是处罚系统**；禁止自动罚款/扣分/绩效处罚设计
- Risk Score 是"安全风险"，不是 Driver Penalty Score
- 单次急刹/哈欠/低头不自动归责司机；无信息时责任 = `UNKNOWN`

任务清单（进度追踪在根目录 `TODO_OFFLINE.md`）：

| # | 任务 | 产出 |
|---|------|------|
| A | RV1106 Product Glue Layer | `dms_product_bridge.h/.c` + `tools/dms_result_replay.py` + 单测 |
| B | STM32 车辆运动节点 | `vehicle_mcu/`：IMU 标定/互补滤波/重力补偿/jerk/事件状态机（阈值全在 config，仅工程初值） |
| B2 | CAN 抽象 | `vehicle_can_state_t` + stub，禁止编造 CAN ID |
| 13/14 | 乘客舒适度引擎 | `passenger_comfort.c/.h`，Comfort Index 0~100（内部工程指标） |
| C | 公交事件融合 | `bus_event_fusion/`：30 秒时间线关联，DMS+车辆运动 → Bus Safety Event（带 SUSPECTED 措辞） |
| D/E | 服务器后台 | `server/`：FastAPI + SQLite + pytest（轻量，不部署） |
| F | Web 原型 | 5 页：安全总览 / Driver Care / Passenger Comfort / Route Risk / Event Timeline |
| G/H | 排班风险 + 司机关怀 | 数据结构 + rule engine，输出 REST/SCHEDULE_REVIEW 建议 |
| I | 系统模拟器 | `simulator/`：10 司机 5 车 3 线路，8 小时运营模拟，POST 到 FastAPI |
| J~M | 文档与部署包 | 隐私伦理、接口统一、整体架构、`deploy/`（只准备不部署，不动 xiaozhi 容器，不用 8000/8003 端口） |
| N/O | 自动测试与回归 | `run_all_offline_tests.sh` 一条命令全绿 + CSV replay 工具 |
| P | 完成报告 | `OFFLINE_WORK_COMPLETE_20260825.md`，明确区分 OFFLINE DONE / REQUIRES HARDWARE |

完成判据：`./run_all_offline_tests.sh` 全绿 + 模拟器跑通 8 小时运营并可在 Web 页面查看结果。

## 4. 明确不属于本次范围（等硬件）

RV1106↔MCU 真实 UART、蜂鸣器 GPIO、STM32 真实 IMU、公交 CAN/DBC、GPS 实车、开机自启实机验证、供电稳定性整改、真人疲劳阈值标定、乘客舒适度阈值标定。这些在报告中一律标记 `PENDING HARDWARE VALIDATION`。

---

> 服务器（101.42.200.249）上已有 xiaozhi-esp32-server（占用 8000/8003，约 745MB 内存），**严禁**停止/重启/覆盖；Bus DMS 服务今天只生成部署包，不启动。
