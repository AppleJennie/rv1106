# server/ - 公交 DMS 后台服务（FastAPI + SQLite）

> 状态：开发版。36/36 pytest 通过。**未部署**，不要在服务器上启动长期服务。
> 红线：本系统是安全提示与关怀系统，不作自动归责；无信息时责任归因 `UNKNOWN`；
> 司机相关推断一律 SUSPECTED 措辞并保留人工复核。无任何处罚/罚款/扣分字段。

## 启动（开发）

```bash
cd server
.venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8099
# 文档: http://127.0.0.1:8099/docs
```

- 端口固定用 **8099** 开发（8000/8003 被 xiaozhi 占用，禁用）。
- 数据库默认 `server/data/app.db`，首次启动自动建表；可用环境变量改路径（见 `app/db.py` 中 `ENV_DB_PATH`）。
- 依赖环境：`server/.venv`（py3.8 + fastapi 0.124.4，见 `requirements.txt`）。
  重建：`python3 -m venv --without-pip server/.venv && server/.venv/bin/python tools/pip.whl/pip install -r server/requirements.txt`（项目根目录执行）。

## 测试

```bash
bash server/run_tests.sh        # 36 passed（ASGI transport，不起真实端口）
```

## API 一览（前缀 /api/v1）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/health` | 健康检查 |
| POST | `/api/v1/drivers` `/vehicles` `/routes` `/shifts` | 注册辅助（Demo 用，返回 id） |
| POST | `/api/v1/dms/events` | DMS 疲劳事件上报 |
| POST | `/api/v1/vehicle/events` | 车辆运动事件上报 |
| POST | `/api/v1/comfort/samples` | 舒适度采样上报 |
| POST | `/api/v1/bus/events` | 融合安全事件上报 |
| GET  | `/api/v1/dashboard/overview` | 总览：各等级车辆计数/今日事件/平均舒适度 |
| GET  | `/api/v1/drivers/{id}/safety` | 司机安全画像 + Driver Care 建议 |
| GET  | `/api/v1/routes/{id}/risk` | 线路风险 |
| GET  | `/api/v1/vehicles/{id}/health` | 车辆健康 |
| GET  | `/api/v1/shifts/{id}/risk` | 班次风险（排班风险引擎） |
| GET  | `/api/v1/events` | 事件联合查询（category 过滤 + 分页） |
| POST | `/api/v1/admin/refresh-daily-summary` | 手动刷新日汇总 |

## 上报 JSON 要点（完整定义见 `app/schemas.py`）

- 时间戳：ISO 8601 字符串（支持结尾 `Z`），所有 ID 字段可空（Demo 阶段）。
- `POST /api/v1/dms/events`：`event_type` ∈ EYE_CLOSED/LONG_EYE_CLOSED/YAWN/HEAD_DOWN/FACE_LOST；`risk_level` ∈ NORMAL/ATTENTION/WARNING/HIGH；可选 ear/mar/head_pitch/duration_ms/snapshot_path/video_path。
- `POST /api/v1/vehicle/events`：`event_type` ∈ HARD_ACCEL/HARD_BRAKE/HARD_TURN_LEFT/HARD_TURN_RIGHT/BUMP/HIGH_LONG_JERK/HIGH_LAT_JERK；`confidence` 0.0~1.0；可选 accel_x/y/z、jerk_long/jerk_lat。
- `POST /api/v1/comfort/samples`：`comfort_score` 0~100 必填；可选 rms_accel/jerk_rms/sample_count。
- `POST /api/v1/bus/events`：`attribution` ∈ UNKNOWN/PEDESTRIAN_AVOIDANCE/TRAFFIC/DRIVER_ATTENTION/ROAD_CONDITION/VEHICLE（默认 UNKNOWN）；可选 dms_event_id/motion_event_id/description。

> ⚠️ 注意：本实现中 vehicle `confidence` 为 **0.0~1.0**，而 `docs/SYSTEM_INTERFACES.md`
> 接口契约写的是 0~100。对接时以本文件/schemas.py 为准，或统一改契约文档（见 `docs/README.md` 假设清单）。

## 业务引擎

- `app/schedule_risk.py`：排班风险 rule engine。输入连续驾驶时长/休息时长/连续工作日/历史疲劳事件数 → NORMAL/ATTENTION/HIGH + NORMAL/REST_RECOMMENDED/SCHEDULE_REVIEW。阈值为工程默认值，需实车标定。
- `app/driver_care.py`：司机关怀引擎。单次事件仅记录；重复发生 → REST_RECOMMENDED；连续多班次高风险 → SCHEDULE_REVIEW。文案示例："近期疲劳风险主要集中在下午班次，建议检查该班次休息间隔。"

## 目录

```
server/
├── app/  main.py db.py schemas.py config.py schedule_risk.py driver_care.py
├── tests/ test_api.py test_schedule_risk.py test_driver_care.py conftest.py
├── data/  app.db（运行时生成）
├── run_tests.sh
└── requirements.txt
```
