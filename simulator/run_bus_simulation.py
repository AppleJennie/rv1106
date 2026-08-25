#!/usr/bin/env python3
"""公交驾驶员安全与乘客舒适度智能系统 —— 离线运营模拟器。

用途：
- 在虚拟时钟下模拟一个公交车队 8 小时运营（默认压缩时间一次跑完），
  通过 HTTP 向后台服务（server/，默认 http://127.0.0.1:8099）上报：
  注册（司机/车辆/线路/班次）、DMS 疲劳事件、车辆运动事件、舒适度采样、
  融合安全事件；跑完后自动自检（GET /api/v1/events 与 dashboard/overview）。
- --offline-jsonl PATH 模式下不连服务器，全部请求落 JSONL 文件，
  供无服务器环境回放/核对。

红线约束（与 server/ 一致）：
- 本系统是安全提示系统，不是处罚系统；代码与文案中不出现任何处罚类概念。
- 单次事件不归责司机；无信息时 attribution=UNKNOWN；
  司机相关推断一律 SUSPECTED 措辞并保留人工复核。

接口契约以 server/app/schemas.py 为准，特别注意：
- vehicle 事件 confidence 取值 0.0~1.0（不是 0~100）。
- 时间戳为 ISO 8601 字符串（本地朴素时间，可带毫秒）。

运行环境：python3.8+；连服务器模式需要 httpx（用 server/.venv 的 python 运行）。
以下事件概率、舒适度模型参数均为演示用工程初始值，需实车标定后调整。
"""
import argparse
import json
import random
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from typing import Any, Callable, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# 模拟参数（演示用工程初始值，需实车标定）
# ---------------------------------------------------------------------------
TICK_SECONDS = 60                 # 虚拟时钟步长（虚拟秒）
COMFORT_INTERVAL_MIN = 10         # 每辆车舒适度采样间隔（虚拟分钟）
P_BG_MOTION_PER_MIN = 0.008       # 每活跃班次每分钟背景运动事件概率
P_BG_DMS_PER_MIN = 0.004          # 每活跃非案例班次每分钟背景 DMS 事件概率
P_FATIGUE_SEQ_PER_MIN = 0.0025    # 每活跃非案例班次每分钟触发疲劳序列概率
BG_MOTION_WEIGHTS = (             # 背景运动事件类型权重
    ("HARD_BRAKE", 0.25),
    ("HARD_ACCEL", 0.25),
    ("HARD_TURN_LEFT", 0.15),
    ("HARD_TURN_RIGHT", 0.15),
    ("BUMP", 0.20),
)
BG_DMS_TYPES = (                  # 背景 DMS：(类型, 风险等级)
    ("YAWN", "ATTENTION"),
    ("EYE_CLOSED", "WARNING"),
    ("HEAD_DOWN", "WARNING"),
    ("FACE_LOST", "ATTENTION"),
)
COMFORT_BASE = 92.0               # 舒适度基准分
COMFORT_DROP_PER_MOTION = 2.5     # 近 10 分钟每次运动事件对舒适度的影响
COMFORT_JITTER = 4.0              # 舒适度随机抖动幅度
COMFORT_MIN, COMFORT_MAX = 45.0, 100.0

API = "/api/v1"


def iso(dt: datetime, ms: bool = False) -> str:
    """ISO 8601 本地朴素时间字符串（schemas.py 要求，毫秒可选）。"""
    return dt.isoformat(timespec="milliseconds" if ms else "seconds")


# ---------------------------------------------------------------------------
# 输出通道：HTTP 或离线 JSONL
# ---------------------------------------------------------------------------
class HttpSink:
    """向真实后台服务 POST/GET（httpx）。"""

    def __init__(self, server: str, timeout: float = 10.0):
        try:
            import httpx
        except ImportError:
            raise SystemExit(
                "连服务器模式需要 httpx，请用 server/.venv/bin/python 运行本脚本"
            )
        self.server = server.rstrip("/")
        self.client = httpx.Client(base_url=self.server, timeout=timeout)

    def get(self, path: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        resp = self.client.get(path, params=params)
        if resp.status_code >= 400:
            raise SystemExit("GET %s 失败 %s: %s" % (path, resp.status_code, resp.text))
        return resp.json()

    def post(self, path: str, payload: Dict[str, Any]) -> Dict[str, Any]:
        resp = self.client.post(path, json=payload)
        if resp.status_code >= 400:
            raise SystemExit(
                "POST %s 失败 %s: %s\npayload=%s"
                % (path, resp.status_code, resp.text, json.dumps(payload, ensure_ascii=False))
            )
        return resp.json()

    def health(self) -> bool:
        try:
            return self.get("/health").get("status") == "ok"
        except Exception:
            return False


class JsonlSink:
    """离线模式：所有 POST 请求落 JSONL；按资源类型自增合成返回 ID。"""

    _ID_KEYS = (
        ("/drivers", "driver_id"),
        ("/vehicles", "vehicle_id"),
        ("/routes", "route_id"),
        ("/shifts", "shift_id"),
    )

    def __init__(self, path: str):
        self.path = path
        self.fp = open(path, "w", encoding="utf-8")
        self._counters: Counter = Counter()

    def post(self, path: str, payload: Dict[str, Any]) -> Dict[str, Any]:
        line = {
            "wall_time": datetime.now().isoformat(timespec="seconds"),
            "method": "POST",
            "path": path,
            "payload": payload,
        }
        self.fp.write(json.dumps(line, ensure_ascii=False) + "\n")
        self.fp.flush()
        key = path
        for suffix, id_key in self._ID_KEYS:
            if path.endswith(suffix):
                key = suffix
                self._counters[key] += 1
                return {id_key: self._counters[key]}
        self._counters[key] += 1
        return {"id": self._counters[key], "status": "recorded"}

    def get(self, path: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        raise SystemExit("离线 JSONL 模式不支持 GET（自检改用本地计数）")

    def close(self) -> None:
        self.fp.close()


# ---------------------------------------------------------------------------
# 车队模型
# ---------------------------------------------------------------------------
@dataclass
class Shift:
    shift_id: int
    driver_id: int
    vehicle_id: int
    route_id: int
    start: datetime
    end: datetime
    case_driver: bool = False  # 案例司机：DMS 事件只来自剧本，不参与随机背景

    def active(self, now: datetime) -> bool:
        return self.start <= now < self.end


# 剧本事件：(触发时间, 动作)
Scripted = Tuple[datetime, Callable[[], None]]


class BusSimulator:
    def __init__(self, sink, rng: random.Random, start: datetime, hours: float):
        self.sink = sink
        self.rng = rng
        self.start = start
        self.end = start + timedelta(hours=hours)
        self.shifts: List[Shift] = []
        self.scripted: List[Scripted] = []
        self.counts: Counter = Counter()          # 本地计数：dms/motion/bus/comfort/register
        self.type_counts: Counter = Counter()     # 按 event_type 计数
        self.recent_motion: Dict[int, List[datetime]] = defaultdict(list)

    # ---------------- 注册 ----------------

    def register_fleet(self) -> None:
        s = self.sink
        driver_ids, vehicle_ids, route_ids = [], [], []
        for i in range(10):
            r = s.post(API + "/drivers", {
                "name": "司机%02d" % (i + 1),
                "employee_no": "D%04d" % (i + 1),
            })
            driver_ids.append(r["driver_id"])
        for i in range(5):
            r = s.post(API + "/vehicles", {
                "plate_no": "京A%05d" % (i + 1),
                "description": "模拟公交车 %d" % (i + 1),
            })
            vehicle_ids.append(r["vehicle_id"])
        for i in range(3):
            r = s.post(API + "/routes", {
                "route_name": "%d路" % (i + 1),
                "description": "模拟线路 %d" % (i + 1),
            })
            route_ids.append(r["route_id"])
        self.counts["register"] += 18

        day0 = self.start
        # 早班：司机1-5 开 车1-5，06:00-10:00；晚班：司机6-10 接手，10:00-14:00
        # 班次字段（continuous_drive_minutes / rest_minutes / consecutive_work_days）
        # 均为演示用排班初始值，需实车标定。
        for i in range(5):
            self._add_shift(
                driver_ids[i], vehicle_ids[i], route_ids[i % 3],
                day0, day0 + timedelta(hours=4),
                continuous=210.0, rest=30.0, days=2,
                case=(i in (0, 1)),  # 司机1(CASE A)、司机2(CASE B) 为案例司机
            )
        for i in range(5):
            driver_idx = i + 5
            is_case_c = driver_idx == 7  # 司机8：CASE C 长连续驾驶 + 高连班
            self._add_shift(
                driver_ids[driver_idx], vehicle_ids[i], route_ids[i % 3],
                day0 + timedelta(hours=4), day0 + timedelta(hours=8),
                continuous=320.0 if is_case_c else 200.0,
                rest=10.0 if is_case_c else 25.0,
                days=5 if is_case_c else 3,
                case=is_case_c,
            )

    def _add_shift(self, driver_id, vehicle_id, route_id, st, et,
                   continuous, rest, days, case=False) -> None:
        r = self.sink.post(API + "/shifts", {
            "driver_id": driver_id,
            "vehicle_id": vehicle_id,
            "route_id": route_id,
            "shift_start": iso(st),
            "shift_end": iso(et),
            "continuous_drive_minutes": continuous,
            "rest_minutes": rest,
            "consecutive_work_days": days,
        })
        self.counts["register"] += 1
        self.shifts.append(Shift(r["shift_id"], driver_id, vehicle_id, route_id,
                                 st, et, case_driver=case))

    # ---------------- 事件上报封装 ----------------

    def post_dms(self, ts: datetime, sh: Shift, event_type: str, risk_level: str,
                 ms: bool = False, **kw: Any) -> int:
        payload = {
            "event_type": event_type,
            "risk_level": risk_level,
            "driver_id": sh.driver_id,
            "vehicle_id": sh.vehicle_id,
            "route_id": sh.route_id,
            "shift_id": sh.shift_id,
            "timestamp": iso(ts, ms=ms),
        }
        payload.update(kw)
        r = self.sink.post(API + "/dms/events", payload)
        self.counts["dms"] += 1
        self.type_counts[event_type] += 1
        return r.get("id") or 0

    def post_motion(self, ts: datetime, sh: Shift, event_type: str,
                    confidence: float, ms: bool = False, **kw: Any) -> int:
        payload = {
            "event_type": event_type,
            "confidence": round(confidence, 2),  # 0.0~1.0（schemas.py 契约）
            "driver_id": sh.driver_id,
            "vehicle_id": sh.vehicle_id,
            "route_id": sh.route_id,
            "shift_id": sh.shift_id,
            "timestamp": iso(ts, ms=ms),
        }
        payload.update(kw)
        r = self.sink.post(API + "/vehicle/events", payload)
        self.counts["motion"] += 1
        self.type_counts[event_type] += 1
        self.recent_motion[sh.vehicle_id].append(ts)
        return r.get("id") or 0

    def post_comfort(self, ts: datetime, sh: Shift) -> None:
        window_start = ts - timedelta(minutes=COMFORT_INTERVAL_MIN)
        recent = [t for t in self.recent_motion[sh.vehicle_id] if t >= window_start]
        score = (COMFORT_BASE - COMFORT_DROP_PER_MOTION * len(recent)
                 - self.rng.uniform(0.0, COMFORT_JITTER))
        score = max(COMFORT_MIN, min(COMFORT_MAX, score))
        self.sink.post(API + "/comfort/samples", {
            "vehicle_id": sh.vehicle_id,
            "route_id": sh.route_id,
            "shift_id": sh.shift_id,
            "timestamp": iso(ts),
            "comfort_score": round(score, 1),
            "rms_accel": round(0.4 + 0.15 * len(recent) + self.rng.uniform(0, 0.2), 3),
            "jerk_rms": round(0.8 + 0.5 * len(recent) + self.rng.uniform(0, 0.5), 3),
            "sample_count": COMFORT_INTERVAL_MIN * 60,
        })
        self.counts["comfort"] += 1

    def post_bus(self, ts: datetime, sh: Shift, attribution: str, description: str,
                 dms_event_id: Optional[int] = None,
                 motion_event_id: Optional[int] = None) -> int:
        r = self.sink.post(API + "/bus/events", {
            "attribution": attribution,
            "dms_event_id": dms_event_id,
            "motion_event_id": motion_event_id,
            "description": description,
            "driver_id": sh.driver_id,
            "vehicle_id": sh.vehicle_id,
            "route_id": sh.route_id,
            "shift_id": sh.shift_id,
            "timestamp": iso(ts, ms=True),
        })
        self.counts["bus"] += 1
        self.type_counts["BUS:" + attribution] += 1
        return r.get("id") or 0

    # ---------------- 典型案例（固定时间点，与 seed 无关） ----------------

    def build_cases(self) -> None:
        t0 = self.start
        sh_a = self.shifts[0]  # 司机1/车1/1路（早班）
        sh_b = self.shifts[1]  # 司机2/车2/2路（早班）
        sh_c = self.shifts[7]  # 司机8/车3/3路（晚班，长连续驾驶）

        # CASE A（t0+1.5h，默认 07:30）：司机 NORMAL + 单次急刹 → UNKNOWN
        ta = t0 + timedelta(hours=1, minutes=30)

        def case_a(ts: datetime = ta, sh: Shift = sh_a) -> None:
            mid = self.post_motion(ts, sh, "HARD_BRAKE", 0.88, accel_x=-4.8,
                                   jerk_long=-3.2)
            self.post_bus(
                ts + timedelta(seconds=2), sh, "UNKNOWN",
                "CASE A：单次急刹车事件；发生时司机状态 NORMAL，无行人避让/路况等"
                "外部信息，责任归因 UNKNOWN。仅作安全提示记录，单次事件不归责司机。",
                motion_event_id=mid,
            )
        self.scripted.append((ta, case_a))

        # CASE B（t0+3.25h，默认 09:15）：低头后 1 秒内急刹
        # → ATTENTION_RELATED_BRAKE_SUSPECTED（SUSPECTED 措辞，需人工复核）
        tb = t0 + timedelta(hours=3, minutes=15)

        def case_b(ts: datetime = tb, sh: Shift = sh_b) -> None:
            did = self.post_dms(ts - timedelta(milliseconds=800), sh,
                                "HEAD_DOWN", "WARNING", ms=True,
                                head_pitch=25.0, duration_ms=1800)
            mid = self.post_motion(ts, sh, "HARD_BRAKE", 0.91, ms=True,
                                   accel_x=-5.2, jerk_long=-3.8)
            self.post_bus(
                ts + timedelta(seconds=1), sh, "DRIVER_ATTENTION",
                "CASE B：ATTENTION_RELATED_BRAKE_SUSPECTED —— 司机低头后 0.8 秒内"
                "发生急刹车，SUSPECTED 与驾驶注意力相关；该推断仅为安全提示，"
                "需人工复核，不作归责。",
                dms_event_id=did, motion_event_id=mid,
            )
        self.scripted.append((tb - timedelta(milliseconds=800), case_b))

        # CASE C（默认 12:30-13:36）：长连续驾驶 + 重复长闭眼 + 多次哈欠
        # → HIGH 疲劳，建议休息/检查排班
        tc = t0 + timedelta(hours=6, minutes=30)
        plan_c = [
            (0, "YAWN", "ATTENTION", {"mar": 0.68, "duration_ms": 1600}),
            (15, "YAWN", "ATTENTION", {"mar": 0.71, "duration_ms": 1800}),
            (30, "EYE_CLOSED", "WARNING", {"ear": 0.17, "duration_ms": 900}),
            (40, "YAWN", "WARNING", {"mar": 0.74, "duration_ms": 2100}),
            (50, "LONG_EYE_CLOSED", "HIGH", {"ear": 0.11, "duration_ms": 2600}),
            (65, "LONG_EYE_CLOSED", "HIGH", {"ear": 0.10, "duration_ms": 3100}),
        ]
        last_dms_id: Dict[str, int] = {"id": 0}

        for minutes, etype, level, extra in plan_c:
            def one(ts: datetime = tc + timedelta(minutes=minutes), sh: Shift = sh_c,
                    e: str = etype, lv: str = level, kw: Dict[str, Any] = extra) -> None:
                last_dms_id["id"] = self.post_dms(ts, sh, e, lv, **kw)
            self.scripted.append((tc + timedelta(minutes=minutes), one))

        tc_fuse = tc + timedelta(minutes=66)

        def case_c_fuse(ts: datetime = tc_fuse, sh: Shift = sh_c) -> None:
            drive_min = int((ts - sh.start).total_seconds() // 60)
            self.post_bus(
                ts, sh, "UNKNOWN",
                "CASE C：连续驾驶约 %d 分钟，出现 2 次长闭眼与多次哈欠，疲劳风险 "
                "HIGH（SUSPECTED，需人工复核）。建议立即安排司机休息，并检查该班次"
                "排班与休息间隔。本提示为安全关怀信息，单次事件不归责司机。"
                % drive_min,
                dms_event_id=last_dms_id["id"] or None,
            )
        self.scripted.append((tc_fuse, case_c_fuse))

        self.scripted.sort(key=lambda item: item[0])

    # ---------------- 背景随机事件 ----------------

    def roll_background(self, now: datetime, sh: Shift) -> None:
        rng = self.rng
        # 背景运动事件（所有车辆）
        if rng.random() < P_BG_MOTION_PER_MIN * (TICK_SECONDS / 60.0):
            r = rng.random()
            acc = 0.0
            for etype, w in BG_MOTION_WEIGHTS:
                acc += w
                if r <= acc:
                    break
            kw: Dict[str, Any] = {}
            if etype == "HARD_BRAKE":
                kw = {"accel_x": round(-rng.uniform(3.5, 6.0), 2),
                      "jerk_long": round(-rng.uniform(2.0, 4.0), 2)}
            elif etype == "HARD_ACCEL":
                kw = {"accel_x": round(rng.uniform(2.5, 4.5), 2),
                      "jerk_long": round(rng.uniform(1.5, 3.0), 2)}
            elif etype in ("HARD_TURN_LEFT", "HARD_TURN_RIGHT"):
                sign = 1.0 if etype == "HARD_TURN_LEFT" else -1.0
                kw = {"accel_y": round(sign * rng.uniform(2.5, 4.5), 2),
                      "jerk_lat": round(sign * rng.uniform(1.5, 3.0), 2)}
            else:  # BUMP
                kw = {"accel_z": round(rng.uniform(2.0, 5.0), 2)}
            self.post_motion(now, sh, etype, rng.uniform(0.6, 0.95), **kw)

        if sh.case_driver:
            return  # 案例司机的 DMS 只来自剧本

        # 背景单发 DMS 事件
        if rng.random() < P_BG_DMS_PER_MIN * (TICK_SECONDS / 60.0):
            etype, level = BG_DMS_TYPES[rng.randrange(len(BG_DMS_TYPES))]
            kw = {}
            if etype == "YAWN":
                kw = {"mar": round(rng.uniform(0.55, 0.75), 2),
                      "duration_ms": rng.randrange(800, 2200, 100)}
            elif etype == "EYE_CLOSED":
                kw = {"ear": round(rng.uniform(0.15, 0.22), 2),
                      "duration_ms": rng.randrange(500, 1200, 100)}
            elif etype == "HEAD_DOWN":
                kw = {"head_pitch": round(rng.uniform(15.0, 30.0), 1),
                      "duration_ms": rng.randrange(1000, 3000, 100)}
            self.post_dms(now, sh, etype, level, **kw)

        # 背景疲劳序列（哈欠→哈欠→闭眼→长闭眼，8 分钟内演进）
        if rng.random() < P_FATIGUE_SEQ_PER_MIN * (TICK_SECONDS / 60.0):
            seq = [
                (0, "YAWN", "ATTENTION", {"mar": round(rng.uniform(0.6, 0.7), 2),
                                          "duration_ms": 1500}),
                (3, "YAWN", "ATTENTION", {"mar": round(rng.uniform(0.6, 0.72), 2),
                                          "duration_ms": 1700}),
                (5, "EYE_CLOSED", "WARNING", {"ear": round(rng.uniform(0.14, 0.2), 2),
                                              "duration_ms": 800}),
                (8, "LONG_EYE_CLOSED", "HIGH", {"ear": round(rng.uniform(0.09, 0.13), 2),
                                                "duration_ms": 2500}),
            ]
            for minutes, etype, level, kw in seq:
                ts = now + timedelta(minutes=minutes)
                if ts < self.end and sh.active(ts):
                    def one(ts: datetime = ts, sh: Shift = sh, e: str = etype,
                            lv: str = level, k: Dict[str, Any] = kw) -> None:
                        self.post_dms(ts, sh, e, lv, **k)
                    self.scripted.append((ts, one))
            self.scripted.sort(key=lambda item: item[0])

    # ---------------- 主循环 ----------------

    def run(self, realtime: bool = False, speed: float = 1.0) -> None:
        self.register_fleet()
        self.build_cases()
        wall0 = time.monotonic()
        now = self.start
        idx = 0
        last_hour_mark = -1
        while now < self.end:
            while idx < len(self.scripted) and self.scripted[idx][0] <= now:
                self.scripted[idx][1]()
                idx += 1
            for sh in self.shifts:
                if sh.active(now):
                    self.roll_background(now, sh)
                    elapsed = (now - sh.start).total_seconds() / 60.0
                    if elapsed > 0 and elapsed % COMFORT_INTERVAL_MIN == 0:
                        self.post_comfort(now, sh)
            hour_mark = int((now - self.start).total_seconds() // 3600)
            if hour_mark != last_hour_mark:
                last_hour_mark = hour_mark
                print("[模拟] 虚拟时间 %s（第 %d 小时）" % (iso(now), hour_mark))
            now += timedelta(seconds=TICK_SECONDS)
            if realtime and speed > 0:
                target = wall0 + (now - self.start).total_seconds() / speed
                delay = target - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
        # 跑完剩余的剧本尾部事件（理论上不会出现，防御）
        while idx < len(self.scripted):
            self.scripted[idx][1]()
            idx += 1

    # ---------------- 自检 ----------------

    def self_check(self, offline: bool = False) -> int:
        print("\n========== 自检汇总 ==========")
        print("本地发送计数: dms=%d motion=%d bus=%d comfort=%d register=%d"
              % (self.counts["dms"], self.counts["motion"], self.counts["bus"],
                 self.counts["comfort"], self.counts["register"]))
        print("按事件类型: %s" % json.dumps(dict(self.type_counts),
                                          ensure_ascii=False, sort_keys=True))
        total_local = self.counts["dms"] + self.counts["motion"] + self.counts["bus"]

        if offline:
            print("离线 JSONL 模式：服务器自检跳过，以本地计数为准。")
            ok = total_local > 0
            print("自检结果: %s（本地事件总数 %d）" % ("通过" if ok else "失败", total_local))
            return 0 if ok else 1

        s = self.sink
        all_ev = s.get(API + "/events", params={"page_size": 1})
        dms_ev = s.get(API + "/events", params={"category": "dms", "page_size": 1})
        mot_ev = s.get(API + "/events", params={"category": "motion", "page_size": 1})
        bus_ev = s.get(API + "/events", params={"category": "bus", "page_size": 1})
        overview = s.get(API + "/dashboard/overview")

        print("服务器 /api/v1/events: total=%d (dms=%d motion=%d bus=%d)"
              % (all_ev["total"], dms_ev["total"], mot_ev["total"], bus_ev["total"]))
        print("服务器 /api/v1/dashboard/overview 原始返回:")
        print(json.dumps(overview, ensure_ascii=False, indent=2))

        mismatch = []
        if dms_ev["total"] != self.counts["dms"]:
            mismatch.append("dms 本地=%d 服务器=%d" % (self.counts["dms"], dms_ev["total"]))
        if mot_ev["total"] != self.counts["motion"]:
            mismatch.append("motion 本地=%d 服务器=%d" % (self.counts["motion"], mot_ev["total"]))
        if bus_ev["total"] != self.counts["bus"]:
            mismatch.append("bus 本地=%d 服务器=%d" % (self.counts["bus"], bus_ev["total"]))

        if all_ev["total"] == 0:
            print("自检结果: 失败 —— 服务器事件数为 0")
            return 1
        if mismatch:
            print("自检结果: 失败 —— 本地/服务器计数不一致: %s" % "; ".join(mismatch))
            return 1
        print("自检结果: 通过（服务器事件总数 %d，与本地一致）" % all_ev["total"])
        return 0


def parse_start(text: str) -> datetime:
    """--start 支持 HH:MM（叠加当天日期）或完整 ISO 8601。"""
    text = text.strip()
    if len(text) == 5 and text[2] == ":":
        hh, mm = text.split(":")
        return datetime.combine(date.today(),
                                datetime.min.time()).replace(hour=int(hh), minute=int(mm))
    return datetime.fromisoformat(text)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="公交系统离线运营模拟器（安全提示系统，非处罚系统）")
    parser.add_argument("--server", default="http://127.0.0.1:8099",
                        help="后台服务地址（默认 http://127.0.0.1:8099）")
    parser.add_argument("--hours", type=float, default=8.0, help="模拟时长（小时，默认 8）")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="--realtime 下虚拟时间相对真实时间的倍速（默认 1.0）")
    parser.add_argument("--seed", type=int, default=42, help="随机种子（默认 42，可复现）")
    parser.add_argument("--start", default="06:00",
                        help="虚拟开始时间 HH:MM 或 ISO 8601（默认 06:00，当天）")
    parser.add_argument("--realtime", action="store_true",
                        help="按真实时间推进（配合 --speed）；默认压缩时间一次跑完")
    parser.add_argument("--offline-jsonl", metavar="PATH",
                        help="离线模式：不写服务器，全部 POST 请求落 JSONL 文件")
    args = parser.parse_args(argv)

    start = parse_start(args.start)
    rng = random.Random(args.seed)

    if args.offline_jsonl:
        sink = JsonlSink(args.offline_jsonl)
        print("[模拟] 离线 JSONL 模式，输出文件: %s" % args.offline_jsonl)
        offline = True
    else:
        sink = HttpSink(args.server)
        if not sink.health():
            raise SystemExit("后台服务不可达: %s（请先启动 server/，端口 8099）" % args.server)
        print("[模拟] 已连接后台服务: %s" % args.server)
        offline = False

    sim = BusSimulator(sink, rng, start, args.hours)
    print("[模拟] 虚拟时钟 %s → %s（%.1f 小时，seed=%d，%s）"
          % (iso(start), iso(sim.end), args.hours, args.seed,
             "实时×%g" % args.speed if args.realtime else "压缩时间"))
    sim.run(realtime=args.realtime, speed=args.speed)
    rc = sim.self_check(offline=offline)
    if offline:
        sink.close()
        print("[模拟] JSONL 已写入: %s" % args.offline_jsonl)
    return rc


if __name__ == "__main__":
    sys.exit(main())
