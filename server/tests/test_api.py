"""API 端点测试：覆盖全部端点与主要校验分支（ASGI transport，不起真实端口）。"""
import os
import re
from datetime import date, datetime, timedelta

DMS_TYPES = ["EYE_CLOSED", "LONG_EYE_CLOSED", "YAWN", "HEAD_DOWN", "FACE_LOST"]
RISK_LEVELS = ["NORMAL", "ATTENTION", "WARNING", "HIGH"]
MOTION_TYPES = ["HARD_ACCEL", "HARD_BRAKE", "HARD_TURN_LEFT", "HARD_TURN_RIGHT",
                "BUMP", "HIGH_LONG_JERK", "HIGH_LAT_JERK"]
ATTRIBUTIONS = ["UNKNOWN", "PEDESTRIAN_AVOIDANCE", "TRAFFIC",
                "DRIVER_ATTENTION", "ROAD_CONDITION", "VEHICLE"]


def _now(offset_minutes=0):
    return (datetime.now() + timedelta(minutes=offset_minutes)).isoformat(timespec="seconds")


async def test_health(client):
    async with client as c:
        r = await c.get("/health")
    assert r.status_code == 200
    body = r.json()
    assert body["status"] == "ok"
    assert "time" in body


async def test_dms_events_all_types_and_levels(client):
    async with client as c:
        for i, et in enumerate(DMS_TYPES):
            r = await c.post("/api/v1/dms/events", json={
                "event_type": et,
                "risk_level": RISK_LEVELS[i % len(RISK_LEVELS)],
                "timestamp": _now(),
                "ear": 0.21, "mar": 0.55, "head_pitch": -12.0, "duration_ms": 800,
                "snapshot_path": "snap/001.jpg", "video_path": "video/001.mp4",
            })
            assert r.status_code == 201, r.text
            assert r.json()["status"] == "recorded"


async def test_dms_events_null_ids_and_default_timestamp(client):
    async with client as c:
        # 全部 ID 为空、不提供 timestamp（服务端补当前时间）
        r = await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "WARNING",
        })
        assert r.status_code == 201
        r = await c.get("/api/v1/events", params={"category": "dms"})
        item = r.json()["items"][0]
        assert item["driver_id"] is None and item["vehicle_id"] is None
        assert item["timestamp"]


async def test_dms_events_validation_errors(client):
    async with client as c:
        r = await c.post("/api/v1/dms/events", json={
            "event_type": "SLEEPING", "risk_level": "HIGH"})
        assert r.status_code == 422
        r = await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "CRITICAL"})
        assert r.status_code == 422
        r = await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "HIGH", "timestamp": "not-a-time"})
        assert r.status_code == 422


async def test_vehicle_motion_events(client):
    async with client as c:
        for et in MOTION_TYPES:
            r = await c.post("/api/v1/vehicle/events", json={
                "event_type": et,
                "confidence": 0.9,
                "accel_x": 0.1, "accel_y": -4.2, "accel_z": 9.8,
                "jerk_long": 6.5, "jerk_lat": 1.2,
                "vehicle_id": 1,
                "timestamp": _now(),
            })
            assert r.status_code == 201, r.text
        r = await c.post("/api/v1/vehicle/events", json={"event_type": "DRIFT"})
        assert r.status_code == 422
        r = await c.post("/api/v1/vehicle/events", json={
            "event_type": "HARD_BRAKE", "confidence": 1.5})
        assert r.status_code == 422  # confidence 越界


async def test_comfort_samples(client):
    async with client as c:
        r = await c.post("/api/v1/comfort/samples", json={
            "vehicle_id": 1, "route_id": 2,
            "comfort_score": 78.5, "rms_accel": 0.8, "jerk_rms": 1.1,
            "sample_count": 150, "timestamp": _now(),
        })
        assert r.status_code == 201
        r = await c.post("/api/v1/comfort/samples", json={"comfort_score": 120})
        assert r.status_code == 422  # 超出 0~100


async def test_bus_events_all_attributions(client):
    async with client as c:
        for att in ATTRIBUTIONS:
            r = await c.post("/api/v1/bus/events", json={
                "attribution": att,
                "vehicle_id": 1,
                "description": "SUSPECTED: 待人工复核" if att == "DRIVER_ATTENTION" else None,
                "timestamp": _now(),
            })
            assert r.status_code == 201, r.text
        # 默认 UNKNOWN
        r = await c.post("/api/v1/bus/events", json={"vehicle_id": 1})
        assert r.status_code == 201
        r = await c.get("/api/v1/events", params={"category": "bus"})
        atts = [i["attribution"] for i in r.json()["items"]]
        assert "UNKNOWN" in atts
        r = await c.post("/api/v1/bus/events", json={"attribution": "DRIVER_FAULT"})
        assert r.status_code == 422


async def test_dashboard_overview(client):
    today = date.today().isoformat()
    async with client as c:
        # 车辆 1：先 ATTENTION 后 HIGH → 当前 HIGH；车辆 2：NORMAL → 当前 NORMAL
        await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "ATTENTION",
            "vehicle_id": 1, "timestamp": today + "T08:00:00"})
        await c.post("/api/v1/dms/events", json={
            "event_type": "LONG_EYE_CLOSED", "risk_level": "HIGH",
            "vehicle_id": 1, "timestamp": today + "T09:00:00"})
        await c.post("/api/v1/dms/events", json={
            "event_type": "EYE_CLOSED", "risk_level": "NORMAL",
            "vehicle_id": 2, "timestamp": today + "T09:30:00"})
        # 历史（昨天）HIGH 不计入今日
        yesterday = (date.today() - timedelta(days=1)).isoformat()
        await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "HIGH",
            "vehicle_id": 3, "timestamp": yesterday + "T09:00:00"})
        await c.post("/api/v1/vehicle/events", json={
            "event_type": "HARD_BRAKE", "vehicle_id": 1, "timestamp": _now()})
        await c.post("/api/v1/vehicle/events", json={
            "event_type": "BUMP", "vehicle_id": 2, "timestamp": _now()})
        await c.post("/api/v1/comfort/samples", json={
            "vehicle_id": 1, "comfort_score": 80.0, "timestamp": _now()})
        await c.post("/api/v1/comfort/samples", json={
            "vehicle_id": 2, "comfort_score": 60.0, "timestamp": _now()})

        r = await c.get("/api/v1/dashboard/overview")
    assert r.status_code == 200
    body = r.json()
    assert body["date"] == today
    assert body["vehicle_status_counts"] == {"NORMAL": 1, "WARNING": 0, "HIGH": 1}
    assert body["today_fatigue_high_count"] == 1
    assert body["today_motion_event_count"] == 2
    assert abs(body["avg_comfort_score"] - 70.0) < 1e-6


async def test_registry_and_shift_risk(client):
    async with client as c:
        d = (await c.post("/api/v1/drivers", json={"name": "张三"})).json()
        v = (await c.post("/api/v1/vehicles", json={"plate_no": "粤B00001"})).json()
        rt = (await c.post("/api/v1/routes", json={"route_name": "M001"})).json()
        s = await c.post("/api/v1/shifts", json={
            "driver_id": d["driver_id"], "vehicle_id": v["vehicle_id"],
            "route_id": rt["route_id"],
            "shift_start": "14:00", "shift_end": "23:30",
            "continuous_drive_minutes": 400, "rest_minutes": 10,
            "consecutive_work_days": 3,
        })
        assert s.status_code == 201, s.text
        shift_id = s.json()["shift_id"]

        r = await c.get("/api/v1/shifts/%d/risk" % shift_id)
        assert r.status_code == 200
        body = r.json()
        # 连续驾驶 400 分钟 >= 360 → HIGH + SCHEDULE_REVIEW
        assert body["schedule_risk_level"] == "HIGH"
        assert body["recommendation"] == "SCHEDULE_REVIEW"
        assert body["reasons"]
        assert "message" in body

        # 正常排班 → NORMAL
        s2 = await c.post("/api/v1/shifts", json={
            "shift_start": "08:00", "shift_end": "16:00",
            "continuous_drive_minutes": 200, "rest_minutes": 30,
            "consecutive_work_days": 2,
        })
        r2 = await c.get("/api/v1/shifts/%d/risk" % s2.json()["shift_id"])
        assert r2.json()["schedule_risk_level"] == "NORMAL"
        assert r2.json()["recommendation"] == "NORMAL"

        # 不存在的班次 → 404
        r3 = await c.get("/api/v1/shifts/9999/risk")
        assert r3.status_code == 404


async def test_shift_risk_writes_summary(client):
    async with client as c:
        s = await c.post("/api/v1/shifts", json={
            "shift_start": "09:00", "shift_end": "17:00",
            "continuous_drive_minutes": 100, "rest_minutes": 60,
        })
        r = await c.get("/api/v1/shifts/%d/risk" % s.json()["shift_id"])
        assert r.status_code == 200
        rows = await c.get("/api/v1/events", params={"category": "dms"})
        assert rows.status_code == 200
    # 直接查库验证 schedule_risk_summary 落行
    from app import db as db_mod
    with db_mod.session() as conn:
        n = conn.execute("SELECT COUNT(*) AS c FROM schedule_risk_summary").fetchone()["c"]
    assert n == 1


async def test_driver_safety_profile_and_care(client):
    async with client as c:
        d = (await c.post("/api/v1/drivers", json={"name": "李四"})).json()
        driver_id = d["driver_id"]
        # 观察窗内 3 次疲劳提示 → REST_RECOMMENDED
        for i in range(3):
            await c.post("/api/v1/dms/events", json={
                "event_type": "EYE_CLOSED", "risk_level": "WARNING",
                "driver_id": driver_id, "shift_id": 1,
                "timestamp": _now(-i * 30)})
        r = await c.get("/api/v1/drivers/%d/safety" % driver_id)
    assert r.status_code == 200
    body = r.json()
    profile = body["driver_safety_profile"]
    assert profile["driver_id"] == driver_id
    assert profile["registered"] is True
    assert profile["total_dms_events"] == 3
    assert profile["by_risk_level"] == {"WARNING": 3}
    assert profile["by_event_type"] == {"EYE_CLOSED": 3}
    assert profile["recent_fatigue_event_count"] == 3
    assert body["care"]["care_level"] == "REST_RECOMMENDED"


async def test_driver_safety_consecutive_high_risk_shifts(client):
    async with client as c:
        driver_id = 7
        today = date.today().isoformat()
        # 班次 11、12 各 1 起 HIGH（连续两个高风险班次），班次 10 无 HIGH
        await c.post("/api/v1/dms/events", json={
            "event_type": "LONG_EYE_CLOSED", "risk_level": "HIGH",
            "driver_id": driver_id, "shift_id": 12, "timestamp": today + "T15:00:00"})
        await c.post("/api/v1/dms/events", json={
            "event_type": "LONG_EYE_CLOSED", "risk_level": "HIGH",
            "driver_id": driver_id, "shift_id": 11, "timestamp": today + "T10:00:00"})
        await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "ATTENTION",
            "driver_id": driver_id, "shift_id": 10, "timestamp": today + "T08:00:00"})
        r = await c.get("/api/v1/drivers/%d/safety" % driver_id)
    body = r.json()
    assert body["driver_safety_profile"]["consecutive_high_risk_shifts"] == 2
    assert body["care"]["care_level"] == "SCHEDULE_REVIEW"
    assert "SUSPECTED" in body["review_note"]


async def test_driver_safety_unknown_driver(client):
    async with client as c:
        r = await c.get("/api/v1/drivers/4242/safety")
    assert r.status_code == 200
    body = r.json()
    assert body["driver_safety_profile"]["registered"] is False
    assert body["driver_safety_profile"]["total_dms_events"] == 0
    assert body["care"]["care_level"] == "NORMAL"


async def test_route_risk(client):
    async with client as c:
        rt = (await c.post("/api/v1/routes", json={"route_name": "M002"})).json()
        route_id = rt["route_id"]
        for i in range(3):
            await c.post("/api/v1/dms/events", json={
                "event_type": "HEAD_DOWN", "risk_level": "HIGH",
                "route_id": route_id, "timestamp": _now(-i)})
        r = await c.get("/api/v1/routes/%d/risk" % route_id)
        assert r.json()["route_risk_level"] == "HIGH"
        assert r.json()["registered"] is True

        # 无事件线路 → NORMAL
        r2 = await c.get("/api/v1/routes/9999/risk")
        assert r2.json()["route_risk_level"] == "NORMAL"
        assert r2.json()["registered"] is False


async def test_vehicle_health(client):
    async with client as c:
        v = (await c.post("/api/v1/vehicles", json={"plate_no": "粤B00002"})).json()
        vehicle_id = v["vehicle_id"]
        await c.post("/api/v1/vehicle/events", json={
            "event_type": "HARD_ACCEL", "vehicle_id": vehicle_id, "timestamp": _now()})
        await c.post("/api/v1/comfort/samples", json={
            "vehicle_id": vehicle_id, "comfort_score": 66.0, "timestamp": _now()})
        r = await c.get("/api/v1/vehicles/%d/health" % vehicle_id)
    body = r.json()
    assert body["health_status"] == "ATTENTION"  # 当日 1 起运动事件
    assert body["today_motion_event_count"] == 1
    assert body["today_motion_by_type"] == {"HARD_ACCEL": 1}
    assert abs(body["avg_comfort_score_today"] - 66.0) < 1e-6


async def test_events_list_filter_and_pagination(client):
    today = date.today().isoformat()
    async with client as c:
        for i in range(3):
            await c.post("/api/v1/dms/events", json={
                "event_type": "YAWN" if i == 0 else "EYE_CLOSED",
                "risk_level": "WARNING",
                "vehicle_id": 1, "timestamp": today + "T0%d:00:00" % (i + 1)})
        await c.post("/api/v1/vehicle/events", json={
            "event_type": "HARD_BRAKE", "vehicle_id": 1, "timestamp": today + "T04:00:00"})
        await c.post("/api/v1/bus/events", json={
            "attribution": "TRAFFIC", "vehicle_id": 1, "timestamp": today + "T05:00:00"})

        r = await c.get("/api/v1/events")
        body = r.json()
        assert body["total"] == 5
        assert len(body["items"]) == 5  # 默认 page_size=20
        # 倒序：最新在前
        assert body["items"][0]["timestamp"] >= body["items"][-1]["timestamp"]

        # 分页
        r = await c.get("/api/v1/events", params={"page": 2, "page_size": 2})
        body = r.json()
        assert body["total"] == 5 and len(body["items"]) == 2 and body["page"] == 2

        # 类型过滤
        r = await c.get("/api/v1/events", params={"event_type": "YAWN"})
        assert r.json()["total"] == 1
        # 类别过滤
        r = await c.get("/api/v1/events", params={"category": "motion"})
        assert r.json()["total"] == 1
        r = await c.get("/api/v1/events", params={"category": "bad"})
        assert r.status_code == 422
        # 时间过滤（仅日期）
        r = await c.get("/api/v1/events", params={
            "start": today, "end": today})
        assert r.json()["total"] == 5
        r = await c.get("/api/v1/events", params={
            "start": (date.today() + timedelta(days=1)).isoformat()})
        assert r.json()["total"] == 0


async def test_refresh_daily_summary(client):
    async with client as c:
        await c.post("/api/v1/dms/events", json={
            "event_type": "YAWN", "risk_level": "HIGH", "timestamp": _now()})
        await c.post("/api/v1/vehicle/events", json={
            "event_type": "BUMP", "timestamp": _now()})
        await c.post("/api/v1/comfort/samples", json={
            "comfort_score": 70.0, "timestamp": _now()})
        r = await c.post("/api/v1/admin/refresh-daily-summary")
    assert r.status_code == 200
    body = r.json()
    assert body["date"] == date.today().isoformat()
    assert body["dms_event_count"] == 1
    assert body["high_risk_event_count"] == 1
    assert body["motion_event_count"] == 1
    assert abs(body["avg_comfort_score"] - 70.0) < 1e-6


def test_no_banned_words_in_app_source():
    """产品红线：app/ 源码不得出现归责/处罚类措辞。"""
    banned = re.compile(
        r"penalty|fine|punish|deduct|罚款|扣分|处罚|绩效考核", re.IGNORECASE)
    app_dir = os.path.join(os.path.dirname(__file__), os.pardir, "app")
    offenders = []
    for fname in os.listdir(app_dir):
        if not fname.endswith(".py"):
            continue
        with open(os.path.join(app_dir, fname), encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                if banned.search(line):
                    offenders.append("%s:%d" % (fname, lineno))
    assert not offenders, "红线措辞出现在: %s" % ", ".join(offenders)
