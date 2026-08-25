"""请求体模型（pydantic v2，兼容 py3.8）。

约定：
- 所有 ID 字段允许为空（Demo 阶段）。
- timestamp 使用 ISO 8601（如 2026-08-24T14:30:00，支持结尾 Z）；
  班次时间（shift_start/shift_end）额外允许 HH:MM。
"""
from datetime import datetime
from enum import Enum
from typing import Optional

from pydantic import BaseModel, Field, field_validator


class DmsEventType(str, Enum):
    EYE_CLOSED = "EYE_CLOSED"
    LONG_EYE_CLOSED = "LONG_EYE_CLOSED"
    YAWN = "YAWN"
    HEAD_DOWN = "HEAD_DOWN"
    FACE_LOST = "FACE_LOST"


class RiskLevel(str, Enum):
    NORMAL = "NORMAL"
    ATTENTION = "ATTENTION"
    WARNING = "WARNING"
    HIGH = "HIGH"


class MotionEventType(str, Enum):
    HARD_ACCEL = "HARD_ACCEL"
    HARD_BRAKE = "HARD_BRAKE"
    HARD_TURN_LEFT = "HARD_TURN_LEFT"
    HARD_TURN_RIGHT = "HARD_TURN_RIGHT"
    BUMP = "BUMP"
    HIGH_LONG_JERK = "HIGH_LONG_JERK"
    HIGH_LAT_JERK = "HIGH_LAT_JERK"


class Attribution(str, Enum):
    UNKNOWN = "UNKNOWN"  # 无信息时责任归因固定为 UNKNOWN
    PEDESTRIAN_AVOIDANCE = "PEDESTRIAN_AVOIDANCE"
    TRAFFIC = "TRAFFIC"
    DRIVER_ATTENTION = "DRIVER_ATTENTION"  # 司机相关推断：仅作 SUSPECTED 提示，需人工复核
    ROAD_CONDITION = "ROAD_CONDITION"
    VEHICLE = "VEHICLE"


def normalize_timestamp(v: str, allow_time_only: bool = False) -> str:
    """校验并原样返回时间字符串；非法格式抛 ValueError（FastAPI 转 422）。"""
    s = v.strip()
    candidate = s[:-1] + "+00:00" if s.endswith("Z") else s
    try:
        datetime.fromisoformat(candidate)
        return s
    except ValueError:
        pass
    if allow_time_only:
        try:
            datetime.strptime(s, "%H:%M")
            return s
        except ValueError:
            pass
    raise ValueError("时间需为 ISO 8601 格式，如 2026-08-24T14:30:00")


class _Timestamped(BaseModel):
    timestamp: Optional[str] = None

    @field_validator("timestamp")
    @classmethod
    def _check_timestamp(cls, v: Optional[str]) -> Optional[str]:
        if v is None:
            return v
        return normalize_timestamp(v)


class DmsEventIn(_Timestamped):
    event_type: DmsEventType
    risk_level: RiskLevel
    driver_id: Optional[int] = None
    vehicle_id: Optional[int] = None
    route_id: Optional[int] = None
    shift_id: Optional[int] = None
    ear: Optional[float] = None
    mar: Optional[float] = None
    head_pitch: Optional[float] = None
    duration_ms: Optional[int] = None
    snapshot_path: Optional[str] = None
    video_path: Optional[str] = None


class MotionEventIn(_Timestamped):
    event_type: MotionEventType
    confidence: Optional[float] = Field(None, ge=0.0, le=1.0)
    accel_x: Optional[float] = None
    accel_y: Optional[float] = None
    accel_z: Optional[float] = None
    jerk_long: Optional[float] = None
    jerk_lat: Optional[float] = None
    driver_id: Optional[int] = None
    vehicle_id: Optional[int] = None
    route_id: Optional[int] = None
    shift_id: Optional[int] = None


class ComfortSampleIn(_Timestamped):
    vehicle_id: Optional[int] = None
    route_id: Optional[int] = None
    shift_id: Optional[int] = None
    comfort_score: float = Field(..., ge=0.0, le=100.0)
    rms_accel: Optional[float] = None
    jerk_rms: Optional[float] = None
    sample_count: Optional[int] = None


class BusSafetyEventIn(_Timestamped):
    attribution: Attribution = Attribution.UNKNOWN
    dms_event_id: Optional[int] = None
    motion_event_id: Optional[int] = None
    description: Optional[str] = None
    snapshot_path: Optional[str] = None
    video_path: Optional[str] = None
    driver_id: Optional[int] = None
    vehicle_id: Optional[int] = None
    route_id: Optional[int] = None
    shift_id: Optional[int] = None


class DriverIn(BaseModel):
    name: Optional[str] = None
    employee_no: Optional[str] = None


class VehicleIn(BaseModel):
    plate_no: Optional[str] = None
    description: Optional[str] = None


class RouteIn(BaseModel):
    route_name: Optional[str] = None
    description: Optional[str] = None


class ShiftIn(BaseModel):
    driver_id: Optional[int] = None
    vehicle_id: Optional[int] = None
    route_id: Optional[int] = None
    shift_start: Optional[str] = None
    shift_end: Optional[str] = None
    continuous_drive_minutes: float = 0.0
    rest_minutes: float = 0.0
    consecutive_work_days: int = 0

    @field_validator("shift_start", "shift_end")
    @classmethod
    def _check_shift_time(cls, v: Optional[str]) -> Optional[str]:
        if v is None:
            return v
        return normalize_timestamp(v, allow_time_only=True)
