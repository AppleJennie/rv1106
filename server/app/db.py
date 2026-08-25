"""SQLite 薄封装（标准库 sqlite3，无 ORM，降低 py3.8 依赖风险）。

开发库说明：
- 所有 ID（driver_id / vehicle_id / route_id / shift_id）允许为空（Demo 阶段，
  设备侧可能无身份/线路信息）。
- 事件行仅保存 snapshot_path / video_path 元数据，不保存文件本体。
- 驾驶表现相关命名统一使用 driver_safety_profile；本系统为安全提示系统，
  不提供任何归责类字段。
- 数据库路径：环境变量 DMS_SERVER_DB 优先，默认 server/data/app.db。
"""
import os
import sqlite3
import threading
from contextlib import contextmanager
from datetime import datetime
from typing import Any, Dict, Iterator, List, Optional

ENV_DB_PATH = "DMS_SERVER_DB"
DEFAULT_DB_PATH = os.path.abspath(
    os.path.join(os.path.dirname(__file__), os.pardir, "data", "app.db")
)

# 允许写入的表名白名单（insert_row 防注入）
TABLES = (
    "drivers",
    "vehicles",
    "routes",
    "shifts",
    "dms_events",
    "vehicle_motion_events",
    "bus_safety_events",
    "comfort_trips",
    "risk_daily_summary",
    "schedule_risk_summary",
)

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS drivers (
    driver_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT,
    employee_no TEXT,
    created_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS vehicles (
    vehicle_id  INTEGER PRIMARY KEY AUTOINCREMENT,
    plate_no    TEXT,
    description TEXT,
    created_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS routes (
    route_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    route_name  TEXT,
    description TEXT,
    created_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS shifts (
    shift_id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    driver_id                 INTEGER,          -- 允许为空（Demo 阶段）
    vehicle_id                INTEGER,
    route_id                  INTEGER,
    shift_start               TEXT,             -- ISO 8601 或 HH:MM
    shift_end                 TEXT,
    continuous_drive_minutes  REAL DEFAULT 0,   -- 排班预计连续驾驶时长
    rest_minutes              REAL DEFAULT 0,   -- 班前/班间休息时长
    consecutive_work_days     INTEGER DEFAULT 0,
    created_at                TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS dms_events (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type    TEXT NOT NULL,    -- EYE_CLOSED/LONG_EYE_CLOSED/YAWN/HEAD_DOWN/FACE_LOST
    risk_level    TEXT NOT NULL,    -- NORMAL/ATTENTION/WARNING/HIGH
    driver_id     INTEGER,
    vehicle_id    INTEGER,
    route_id      INTEGER,
    shift_id      INTEGER,
    timestamp     TEXT NOT NULL,    -- ISO 8601
    ear           REAL,
    mar           REAL,
    head_pitch    REAL,
    duration_ms   INTEGER,
    snapshot_path TEXT,             -- 仅元数据
    video_path    TEXT,             -- 仅元数据
    raw_json      TEXT,
    created_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_dms_events_ts ON dms_events(timestamp);
CREATE INDEX IF NOT EXISTS idx_dms_events_driver ON dms_events(driver_id);

CREATE TABLE IF NOT EXISTS vehicle_motion_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type  TEXT NOT NULL,      -- HARD_ACCEL/HARD_BRAKE/HARD_TURN_LEFT/HARD_TURN_RIGHT/BUMP/HIGH_LONG_JERK/HIGH_LAT_JERK
    confidence  REAL,               -- 0.0 ~ 1.0
    accel_x     REAL,
    accel_y     REAL,
    accel_z     REAL,
    jerk_long   REAL,
    jerk_lat    REAL,
    driver_id   INTEGER,
    vehicle_id  INTEGER,
    route_id    INTEGER,
    shift_id    INTEGER,
    timestamp   TEXT NOT NULL,
    raw_json    TEXT,
    created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_motion_ts ON vehicle_motion_events(timestamp);

CREATE TABLE IF NOT EXISTS bus_safety_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    attribution     TEXT NOT NULL DEFAULT 'UNKNOWN',  -- UNKNOWN/PEDESTRIAN_AVOIDANCE/TRAFFIC/DRIVER_ATTENTION/ROAD_CONDITION/VEHICLE
    dms_event_id    INTEGER,          -- 关联 DMS 事件（可空）
    motion_event_id INTEGER,          -- 关联运动事件（可空）
    driver_id       INTEGER,
    vehicle_id      INTEGER,
    route_id        INTEGER,
    shift_id        INTEGER,
    timestamp       TEXT NOT NULL,
    description     TEXT,             -- 司机相关推断必须使用 SUSPECTED 措辞
    snapshot_path   TEXT,             -- 仅元数据
    video_path      TEXT,             -- 仅元数据
    raw_json        TEXT,
    created_at      TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_bus_events_ts ON bus_safety_events(timestamp);

CREATE TABLE IF NOT EXISTS comfort_trips (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    vehicle_id    INTEGER,
    route_id      INTEGER,
    shift_id      INTEGER,
    timestamp     TEXT NOT NULL,
    comfort_score REAL,               -- 0~100，越高越舒适
    rms_accel     REAL,
    jerk_rms      REAL,
    sample_count  INTEGER,
    raw_json      TEXT,
    created_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_comfort_ts ON comfort_trips(timestamp);

CREATE TABLE IF NOT EXISTS risk_daily_summary (
    id                    INTEGER PRIMARY KEY AUTOINCREMENT,
    date                  TEXT NOT NULL,        -- YYYY-MM-DD
    driver_id             INTEGER,              -- Demo：当前仅聚合全局行（ID 全空）
    vehicle_id            INTEGER,
    route_id              INTEGER,
    dms_event_count       INTEGER DEFAULT 0,
    high_risk_event_count INTEGER DEFAULT 0,
    motion_event_count    INTEGER DEFAULT 0,
    avg_comfort_score     REAL,
    driver_safety_profile TEXT,                 -- JSON 摘要（驾驶表现画像，仅安全提示用途）
    refreshed_at          TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS schedule_risk_summary (
    id                              INTEGER PRIMARY KEY AUTOINCREMENT,
    shift_id                        INTEGER,
    evaluated_at                    TEXT NOT NULL,
    schedule_risk_level             TEXT NOT NULL,  -- NORMAL/ATTENTION/HIGH
    recommendation                  TEXT NOT NULL,  -- NORMAL/REST_RECOMMENDED/SCHEDULE_REVIEW
    reasons_json                    TEXT,
    continuous_drive_minutes        REAL,
    rest_minutes                    REAL,
    consecutive_work_days           INTEGER,
    historical_fatigue_event_count  INTEGER
);
"""

_init_lock = threading.Lock()
_initialized_paths = set()


def now_iso() -> str:
    """本地时间 ISO 字符串（秒级）。Demo 阶段统一使用本地朴素时间。"""
    return datetime.now().isoformat(timespec="seconds")


def resolve_db_path() -> str:
    return os.environ.get(ENV_DB_PATH) or DEFAULT_DB_PATH


def connect(db_path: Optional[str] = None) -> sqlite3.Connection:
    path = db_path or resolve_db_path()
    if path != ":memory:":
        parent = os.path.dirname(path)
        if parent:
            os.makedirs(parent, exist_ok=True)
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    _ensure_schema(conn, path)
    return conn


def _ensure_schema(conn: sqlite3.Connection, path: str) -> None:
    with _init_lock:
        if path in _initialized_paths:
            return
        conn.executescript(SCHEMA_SQL)
        conn.commit()
        _initialized_paths.add(path)


@contextmanager
def session(db_path: Optional[str] = None) -> Iterator[sqlite3.Connection]:
    conn = connect(db_path)
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def insert_row(conn: sqlite3.Connection, table: str, row: Dict[str, Any]) -> int:
    if table not in TABLES:
        raise ValueError("未知表名: %s" % table)
    cols = ", ".join(row.keys())
    marks = ", ".join("?" for _ in row)
    cur = conn.execute(
        "INSERT INTO %s (%s) VALUES (%s)" % (table, cols, marks), list(row.values())
    )
    return int(cur.lastrowid)


def rows_to_dicts(cur: sqlite3.Cursor) -> List[Dict[str, Any]]:
    return [dict(r) for r in cur.fetchall()]


def refresh_risk_daily_summary(day: str, db_path: Optional[str] = None) -> Dict[str, Any]:
    """聚合指定日期（YYYY-MM-DD）的全局风险日报（Demo：仅 ID 全空的全局行）。

    返回写入的汇总行。夜间批处理/运维接口复用本函数。
    """
    with session(db_path) as conn:
        dms_cnt = conn.execute(
            "SELECT COUNT(*) AS c FROM dms_events WHERE substr(timestamp,1,10)=?", (day,)
        ).fetchone()["c"]
        high_cnt = conn.execute(
            "SELECT COUNT(*) AS c FROM dms_events "
            "WHERE substr(timestamp,1,10)=? AND risk_level='HIGH'",
            (day,),
        ).fetchone()["c"]
        motion_cnt = conn.execute(
            "SELECT COUNT(*) AS c FROM vehicle_motion_events WHERE substr(timestamp,1,10)=?",
            (day,),
        ).fetchone()["c"]
        avg_comfort = conn.execute(
            "SELECT AVG(comfort_score) AS a FROM comfort_trips WHERE substr(timestamp,1,10)=?",
            (day,),
        ).fetchone()["a"]
        conn.execute(
            "DELETE FROM risk_daily_summary WHERE date=? "
            "AND driver_id IS NULL AND vehicle_id IS NULL AND route_id IS NULL",
            (day,),
        )
        row = {
            "date": day,
            "driver_id": None,
            "vehicle_id": None,
            "route_id": None,
            "dms_event_count": dms_cnt,
            "high_risk_event_count": high_cnt,
            "motion_event_count": motion_cnt,
            "avg_comfort_score": avg_comfort,
            "driver_safety_profile": None,
            "refreshed_at": now_iso(),
        }
        row_id = insert_row(conn, "risk_daily_summary", row)
        row["id"] = row_id
        return row
