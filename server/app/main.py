"""公交驾驶员安全与乘客舒适度智能系统 —— 后台服务入口（FastAPI）。

- API 前缀 /api/v1；开发端口 8099（8000/8003 被占用，禁用）。
- 本系统为安全提示与关怀系统：不作自动归责；无信息时责任归因 UNKNOWN；
  司机相关推断一律使用 SUSPECTED 措辞并保留人工复核。
- 所有阈值见 app/config.py、app/schedule_risk.py、app/driver_care.py，
  数值均为仅工程初始值，需实车标定。

启动（开发）：cd server && .venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8099
"""
import json
import os
from datetime import date, datetime, timedelta
from typing import Optional

from fastapi import FastAPI, HTTPException, Query

from . import db
from .config import CARE_WINDOW, DASHBOARD, ROUTE_RISK, VEHICLE_HEALTH
from .driver_care import assess_driver_care, dominant_period
from .schedule_risk import evaluate_shift
from .schemas import (
    BusSafetyEventIn,
    ComfortSampleIn,
    DmsEventIn,
    DriverIn,
    MotionEventIn,
    RouteIn,
    ShiftIn,
    VehicleIn,
)

# 当日 HIGH 等级 DMS 事件按司机/班次聚合时使用的 SQL 片段
_LATEST_PER_VEHICLE_SQL = """
SELECT e.vehicle_id AS vehicle_id, e.risk_level AS risk_level
FROM dms_events e
JOIN (
    SELECT vehicle_id, MAX(timestamp) AS max_ts
    FROM dms_events
    WHERE substr(timestamp, 1, 10) = ? AND vehicle_id IS NOT NULL
    GROUP BY vehicle_id
) t ON e.vehicle_id = t.vehicle_id AND e.timestamp = t.max_ts
"""

_EVENTS_UNION_SQL = """
SELECT 'dms' AS category, id, event_type, risk_level AS level, NULL AS attribution,
       timestamp, driver_id, vehicle_id, route_id, shift_id, snapshot_path, video_path
FROM dms_events
UNION ALL
SELECT 'motion', id, event_type, NULL, NULL,
       timestamp, driver_id, vehicle_id, route_id, shift_id, NULL, NULL
FROM vehicle_motion_events
UNION ALL
SELECT 'bus', id, NULL, NULL, attribution,
       timestamp, driver_id, vehicle_id, route_id, shift_id, snapshot_path, video_path
FROM bus_safety_events
"""


def _today() -> str:
    return date.today().isoformat()


def create_app(db_path: Optional[str] = None) -> FastAPI:
    if db_path:
        os.environ[db.ENV_DB_PATH] = db_path
    app = FastAPI(
        title="公交驾驶员安全与乘客舒适度智能系统 - 后台服务",
        version="0.1.0",
    )
    with db.session():
        pass  # 确保 schema 已初始化

    # ---------------- 健康检查 ----------------

    @app.get("/health")
    def health():
        return {"status": "ok", "time": db.now_iso()}

    # ---------------- 注册接口（Demo 辅助） ----------------

    @app.post("/api/v1/drivers", status_code=201)
    def create_driver(payload: DriverIn):
        with db.session() as conn:
            rid = db.insert_row(conn, "drivers", {
                "name": payload.name,
                "employee_no": payload.employee_no,
                "created_at": db.now_iso(),
            })
        return {"driver_id": rid}

    @app.post("/api/v1/vehicles", status_code=201)
    def create_vehicle(payload: VehicleIn):
        with db.session() as conn:
            rid = db.insert_row(conn, "vehicles", {
                "plate_no": payload.plate_no,
                "description": payload.description,
                "created_at": db.now_iso(),
            })
        return {"vehicle_id": rid}

    @app.post("/api/v1/routes", status_code=201)
    def create_route(payload: RouteIn):
        with db.session() as conn:
            rid = db.insert_row(conn, "routes", {
                "route_name": payload.route_name,
                "description": payload.description,
                "created_at": db.now_iso(),
            })
        return {"route_id": rid}

    @app.post("/api/v1/shifts", status_code=201)
    def create_shift(payload: ShiftIn):
        with db.session() as conn:
            rid = db.insert_row(conn, "shifts", {
                "driver_id": payload.driver_id,
                "vehicle_id": payload.vehicle_id,
                "route_id": payload.route_id,
                "shift_start": payload.shift_start,
                "shift_end": payload.shift_end,
                "continuous_drive_minutes": payload.continuous_drive_minutes,
                "rest_minutes": payload.rest_minutes,
                "consecutive_work_days": payload.consecutive_work_days,
                "created_at": db.now_iso(),
            })
        return {"shift_id": rid}

    # ---------------- 事件上报 ----------------

    @app.post("/api/v1/dms/events", status_code=201)
    def create_dms_event(payload: DmsEventIn):
        row = {
            "event_type": payload.event_type.value,
            "risk_level": payload.risk_level.value,
            "driver_id": payload.driver_id,
            "vehicle_id": payload.vehicle_id,
            "route_id": payload.route_id,
            "shift_id": payload.shift_id,
            "timestamp": payload.timestamp or db.now_iso(),
            "ear": payload.ear,
            "mar": payload.mar,
            "head_pitch": payload.head_pitch,
            "duration_ms": payload.duration_ms,
            "snapshot_path": payload.snapshot_path,
            "video_path": payload.video_path,
            "raw_json": payload.model_dump_json(),
            "created_at": db.now_iso(),
        }
        with db.session() as conn:
            rid = db.insert_row(conn, "dms_events", row)
        return {"id": rid, "status": "recorded"}

    @app.post("/api/v1/vehicle/events", status_code=201)
    def create_motion_event(payload: MotionEventIn):
        row = {
            "event_type": payload.event_type.value,
            "confidence": payload.confidence,
            "accel_x": payload.accel_x,
            "accel_y": payload.accel_y,
            "accel_z": payload.accel_z,
            "jerk_long": payload.jerk_long,
            "jerk_lat": payload.jerk_lat,
            "driver_id": payload.driver_id,
            "vehicle_id": payload.vehicle_id,
            "route_id": payload.route_id,
            "shift_id": payload.shift_id,
            "timestamp": payload.timestamp or db.now_iso(),
            "raw_json": payload.model_dump_json(),
            "created_at": db.now_iso(),
        }
        with db.session() as conn:
            rid = db.insert_row(conn, "vehicle_motion_events", row)
        return {"id": rid, "status": "recorded"}

    @app.post("/api/v1/comfort/samples", status_code=201)
    def create_comfort_sample(payload: ComfortSampleIn):
        row = {
            "vehicle_id": payload.vehicle_id,
            "route_id": payload.route_id,
            "shift_id": payload.shift_id,
            "timestamp": payload.timestamp or db.now_iso(),
            "comfort_score": payload.comfort_score,
            "rms_accel": payload.rms_accel,
            "jerk_rms": payload.jerk_rms,
            "sample_count": payload.sample_count,
            "raw_json": payload.model_dump_json(),
            "created_at": db.now_iso(),
        }
        with db.session() as conn:
            rid = db.insert_row(conn, "comfort_trips", row)
        return {"id": rid, "status": "recorded"}

    @app.post("/api/v1/bus/events", status_code=201)
    def create_bus_event(payload: BusSafetyEventIn):
        row = {
            "attribution": payload.attribution.value,
            "dms_event_id": payload.dms_event_id,
            "motion_event_id": payload.motion_event_id,
            "driver_id": payload.driver_id,
            "vehicle_id": payload.vehicle_id,
            "route_id": payload.route_id,
            "shift_id": payload.shift_id,
            "timestamp": payload.timestamp or db.now_iso(),
            "description": payload.description,
            "snapshot_path": payload.snapshot_path,
            "video_path": payload.video_path,
            "raw_json": payload.model_dump_json(),
            "created_at": db.now_iso(),
        }
        with db.session() as conn:
            rid = db.insert_row(conn, "bus_safety_events", row)
        return {"id": rid, "status": "recorded"}

    # ---------------- 看板 / 画像 ----------------

    @app.get("/api/v1/dashboard/overview")
    def dashboard_overview():
        today = _today()
        with db.session() as conn:
            latest = db.rows_to_dicts(conn.execute(_LATEST_PER_VEHICLE_SQL, (today,)))
            buckets = {"NORMAL": 0, "WARNING": 0, "HIGH": 0}
            for row in latest:
                level = row["risk_level"]
                if level == "HIGH":
                    buckets["HIGH"] += 1
                elif level in ("ATTENTION", "WARNING") and DASHBOARD.attention_counts_as_warning:
                    buckets["WARNING"] += 1
                elif level == "WARNING":
                    buckets["WARNING"] += 1
                else:
                    buckets["NORMAL"] += 1
            high_fatigue = conn.execute(
                "SELECT COUNT(*) AS c FROM dms_events "
                "WHERE substr(timestamp,1,10)=? AND risk_level='HIGH'",
                (today,),
            ).fetchone()["c"]
            motion_cnt = conn.execute(
                "SELECT COUNT(*) AS c FROM vehicle_motion_events WHERE substr(timestamp,1,10)=?",
                (today,),
            ).fetchone()["c"]
            avg_comfort = conn.execute(
                "SELECT AVG(comfort_score) AS a FROM comfort_trips WHERE substr(timestamp,1,10)=?",
                (today,),
            ).fetchone()["a"]
        return {
            "date": today,
            "vehicle_status_counts": buckets,
            "today_fatigue_high_count": high_fatigue,
            "today_motion_event_count": motion_cnt,
            "avg_comfort_score": avg_comfort,
        }

    @app.get("/api/v1/drivers/{driver_id}/safety")
    def driver_safety(driver_id: int):
        """驾驶表现画像（driver_safety_profile，安全提示用途，不作归责）。"""
        since = (datetime.now() - timedelta(hours=CARE_WINDOW.recent_window_hours)
                 ).isoformat(timespec="seconds")
        with db.session() as conn:
            registered = conn.execute(
                "SELECT 1 FROM drivers WHERE driver_id=?", (driver_id,)
            ).fetchone() is not None
            by_risk = {
                r["risk_level"]: r["c"] for r in conn.execute(
                    "SELECT risk_level, COUNT(*) AS c FROM dms_events "
                    "WHERE driver_id=? GROUP BY risk_level", (driver_id,))
            }
            by_type = {
                r["event_type"]: r["c"] for r in conn.execute(
                    "SELECT event_type, COUNT(*) AS c FROM dms_events "
                    "WHERE driver_id=? GROUP BY event_type", (driver_id,))
            }
            last_ts = conn.execute(
                "SELECT MAX(timestamp) AS t FROM dms_events WHERE driver_id=?", (driver_id,)
            ).fetchone()["t"]
            recent_rows = db.rows_to_dicts(conn.execute(
                "SELECT timestamp FROM dms_events WHERE driver_id=? AND timestamp>=? "
                "ORDER BY timestamp", (driver_id, since)))
            shift_rows = db.rows_to_dicts(conn.execute(
                "SELECT shift_id, MAX(timestamp) AS last_ts, "
                "SUM(CASE WHEN risk_level='HIGH' THEN 1 ELSE 0 END) AS high_cnt "
                "FROM dms_events WHERE driver_id=? AND shift_id IS NOT NULL "
                "GROUP BY shift_id ORDER BY last_ts DESC", (driver_id,)))

        consecutive_high = 0
        for row in shift_rows:
            if row["high_cnt"] and row["high_cnt"] > 0:
                consecutive_high += 1
            else:
                break
        focus = dominant_period([r["timestamp"] for r in recent_rows])
        advice = assess_driver_care(len(recent_rows), consecutive_high, focus)
        profile = {
            "driver_id": driver_id,
            "registered": registered,
            "total_dms_events": sum(by_risk.values()),
            "by_risk_level": by_risk,
            "by_event_type": by_type,
            "last_event_at": last_ts,
            "recent_window_hours": CARE_WINDOW.recent_window_hours,
            "recent_fatigue_event_count": len(recent_rows),
            "consecutive_high_risk_shifts": consecutive_high,
        }
        return {
            "driver_safety_profile": profile,
            "care": {
                "care_level": advice.care_level,
                "message": advice.message,
                "reasons": advice.reasons,
            },
            "review_note": "以上为安全提示信息；司机相关推断仅为 SUSPECTED，需人工复核。",
        }

    @app.get("/api/v1/routes/{route_id}/risk")
    def route_risk(route_id: int):
        with db.session() as conn:
            registered = conn.execute(
                "SELECT 1 FROM routes WHERE route_id=?", (route_id,)
            ).fetchone() is not None
            dms_total = conn.execute(
                "SELECT COUNT(*) AS c FROM dms_events WHERE route_id=?", (route_id,)
            ).fetchone()["c"]
            high_cnt = conn.execute(
                "SELECT COUNT(*) AS c FROM dms_events WHERE route_id=? AND risk_level='HIGH'",
                (route_id,),
            ).fetchone()["c"]
            motion_cnt = conn.execute(
                "SELECT COUNT(*) AS c FROM vehicle_motion_events WHERE route_id=?", (route_id,)
            ).fetchone()["c"]
            avg_comfort = conn.execute(
                "SELECT AVG(comfort_score) AS a FROM comfort_trips WHERE route_id=?", (route_id,)
            ).fetchone()["a"]

        if high_cnt >= ROUTE_RISK.high_events_high:
            level = "HIGH"
        elif high_cnt >= ROUTE_RISK.high_events_attention or motion_cnt >= ROUTE_RISK.motion_events_attention:
            level = "ATTENTION"
        else:
            level = "NORMAL"
        return {
            "route_id": route_id,
            "registered": registered,
            "route_risk_level": level,
            "dms_event_count": dms_total,
            "high_risk_event_count": high_cnt,
            "motion_event_count": motion_cnt,
            "avg_comfort_score": avg_comfort,
            "message": {
                "NORMAL": "线路风险未见明显异常，保持关注。",
                "ATTENTION": "线路出现安全风险信号，建议关注该线路时段与路况。",
                "HIGH": "线路安全风险偏高，建议复核该线路排班与休息安排。",
            }[level],
        }

    @app.get("/api/v1/vehicles/{vehicle_id}/health")
    def vehicle_health(vehicle_id: int):
        today = _today()
        with db.session() as conn:
            registered = conn.execute(
                "SELECT 1 FROM vehicles WHERE vehicle_id=?", (vehicle_id,)
            ).fetchone() is not None
            today_motion = conn.execute(
                "SELECT COUNT(*) AS c FROM vehicle_motion_events "
                "WHERE vehicle_id=? AND substr(timestamp,1,10)=?",
                (vehicle_id, today),
            ).fetchone()["c"]
            total_motion = conn.execute(
                "SELECT COUNT(*) AS c FROM vehicle_motion_events WHERE vehicle_id=?",
                (vehicle_id,),
            ).fetchone()["c"]
            by_type = {
                r["event_type"]: r["c"] for r in conn.execute(
                    "SELECT event_type, COUNT(*) AS c FROM vehicle_motion_events "
                    "WHERE vehicle_id=? AND substr(timestamp,1,10)=? GROUP BY event_type",
                    (vehicle_id, today))
            }
            avg_comfort = conn.execute(
                "SELECT AVG(comfort_score) AS a FROM comfort_trips "
                "WHERE vehicle_id=? AND substr(timestamp,1,10)=?",
                (vehicle_id, today),
            ).fetchone()["a"]

        if today_motion >= VEHICLE_HEALTH.today_motion_high:
            status = "HIGH"
        elif today_motion >= VEHICLE_HEALTH.today_motion_attention:
            status = "ATTENTION"
        else:
            status = "NORMAL"
        return {
            "vehicle_id": vehicle_id,
            "registered": registered,
            "health_status": status,
            "today_motion_event_count": today_motion,
            "today_motion_by_type": by_type,
            "total_motion_event_count": total_motion,
            "avg_comfort_score_today": avg_comfort,
        }

    @app.get("/api/v1/shifts/{shift_id}/risk")
    def shift_risk(shift_id: int):
        with db.session() as conn:
            row = conn.execute(
                "SELECT * FROM shifts WHERE shift_id=?", (shift_id,)
            ).fetchone()
            if row is None:
                raise HTTPException(status_code=404, detail="shift 不存在")
            shift = dict(row)
            if shift["driver_id"] is not None:
                hist = conn.execute(
                    "SELECT COUNT(*) AS c FROM dms_events WHERE driver_id=?",
                    (shift["driver_id"],),
                ).fetchone()["c"]
            else:
                hist = conn.execute(
                    "SELECT COUNT(*) AS c FROM dms_events WHERE shift_id=?", (shift_id,)
                ).fetchone()["c"]

            assessment = evaluate_shift(
                continuous_drive_minutes=shift["continuous_drive_minutes"],
                rest_minutes=shift["rest_minutes"],
                shift_start=shift["shift_start"],
                shift_end=shift["shift_end"],
                consecutive_work_days=shift["consecutive_work_days"],
                historical_fatigue_event_count=hist,
            )
            db.insert_row(conn, "schedule_risk_summary", {
                "shift_id": shift_id,
                "evaluated_at": db.now_iso(),
                "schedule_risk_level": assessment.schedule_risk_level,
                "recommendation": assessment.recommendation,
                "reasons_json": json.dumps(assessment.reasons, ensure_ascii=False),
                "continuous_drive_minutes": shift["continuous_drive_minutes"],
                "rest_minutes": shift["rest_minutes"],
                "consecutive_work_days": shift["consecutive_work_days"],
                "historical_fatigue_event_count": hist,
            })
        return {
            "shift_id": shift_id,
            "schedule_risk_level": assessment.schedule_risk_level,
            "recommendation": assessment.recommendation,
            "reasons": assessment.reasons,
            "message": assessment.message,
            "historical_fatigue_event_count": hist,
        }

    # ---------------- 事件查询 ----------------

    @app.get("/api/v1/events")
    def list_events(
        event_type: Optional[str] = None,
        category: Optional[str] = None,
        start: Optional[str] = None,
        end: Optional[str] = None,
        page: int = Query(1, ge=1),
        page_size: int = Query(20, ge=1, le=200),
    ):
        """跨表事件查询：category ∈ dms/motion/bus；start/end 支持 ISO 或 YYYY-MM-DD。"""
        if category is not None and category not in ("dms", "motion", "bus"):
            raise HTTPException(status_code=422, detail="category 仅支持 dms/motion/bus")

        conds, params = [], []
        if event_type:
            conds.append("event_type = ?")
            params.append(event_type)
        if category:
            conds.append("category = ?")
            params.append(category)
        if start:
            conds.append("timestamp >= ?" if len(start) > 10 else "substr(timestamp,1,10) >= ?")
            params.append(start)
        if end:
            conds.append("timestamp <= ?" if len(end) > 10 else "substr(timestamp,1,10) <= ?")
            params.append(end)
        where = (" WHERE " + " AND ".join(conds)) if conds else ""

        with db.session() as conn:
            total = conn.execute(
                "SELECT COUNT(*) AS c FROM (%s)%s" % (_EVENTS_UNION_SQL, where), params
            ).fetchone()["c"]
            items = db.rows_to_dicts(conn.execute(
                "SELECT * FROM (%s)%s ORDER BY timestamp DESC, id DESC LIMIT ? OFFSET ?"
                % (_EVENTS_UNION_SQL, where),
                params + [page_size, (page - 1) * page_size],
            ))
        return {"total": total, "page": page, "page_size": page_size, "items": items}

    # ---------------- 运维辅助（日报聚合） ----------------

    @app.post("/api/v1/admin/refresh-daily-summary")
    def refresh_daily_summary(day: Optional[str] = None):
        target = day or _today()
        row = db.refresh_risk_daily_summary(target)
        return row

    return app


app = create_app()
