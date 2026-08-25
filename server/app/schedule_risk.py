"""TASK G 排班风险规则引擎（规则式，当前全部基于 mock 数据）。

输入：continuous_drive_minutes / rest_minutes / shift_start / shift_end /
      consecutive_work_days / historical_fatigue_event_count
输出：schedule_risk_level（NORMAL/ATTENTION/HIGH）
      + recommendation（NORMAL/REST_RECOMMENDED/SCHEDULE_REVIEW）

所有阈值集中在 ScheduleRiskConfig，数值均为 **仅工程初始值，需实车标定**。
本引擎仅输出安全提示与调度建议，不作任何归责。
"""
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Optional, Tuple

LEVEL_ORDER = {"NORMAL": 0, "ATTENTION": 1, "HIGH": 2}
RECOMMENDATIONS = ("NORMAL", "REST_RECOMMENDED", "SCHEDULE_REVIEW")

_LEVEL_TO_RECOMMENDATION = {
    "NORMAL": "NORMAL",
    "ATTENTION": "REST_RECOMMENDED",
    "HIGH": "SCHEDULE_REVIEW",
}


@dataclass(frozen=True)
class ScheduleRiskConfig:
    """排班风险阈值（以下全部为仅工程初始值，需实车标定）。"""
    max_continuous_drive_minutes_attention: float = 240.0  # 连续驾驶 >= 4h 关注
    max_continuous_drive_minutes_high: float = 360.0       # 连续驾驶 >= 6h 高风险
    min_rest_minutes: float = 20.0                         # 休息不足 20min 关注
    shift_hours_attention: float = 10.0                    # 单班次 >= 10h 关注
    consecutive_work_days_attention: int = 5               # 连续出勤 >= 5 天关注
    consecutive_work_days_high: int = 7                    # 连续出勤 >= 7 天高风险
    night_start_hour_begin: int = 0    # [begin, end) 点之间开班视为凌晨班次
    night_start_hour_end: int = 6
    fatigue_events_attention: int = 3  # 历史疲劳事件 >= 3 起关注
    fatigue_events_high: int = 5       # 历史疲劳事件 >= 5 起高风险


DEFAULT_CONFIG = ScheduleRiskConfig()


@dataclass
class ScheduleRiskAssessment:
    schedule_risk_level: str          # NORMAL / ATTENTION / HIGH
    recommendation: str               # NORMAL / REST_RECOMMENDED / SCHEDULE_REVIEW
    reasons: List[str] = field(default_factory=list)
    message: str = ""


def _parse_dt(ts: Optional[str]) -> Optional[datetime]:
    if not ts:
        return None
    s = ts.strip()
    candidate = s[:-1] + "+00:00" if s.endswith("Z") else s
    try:
        return datetime.fromisoformat(candidate)
    except ValueError:
        pass
    try:
        return datetime.strptime(s, "%H:%M")
    except ValueError:
        return None


def evaluate_shift(
    continuous_drive_minutes: Optional[float] = None,
    rest_minutes: Optional[float] = None,
    shift_start: Optional[str] = None,
    shift_end: Optional[str] = None,
    consecutive_work_days: Optional[int] = None,
    historical_fatigue_event_count: int = 0,
    config: ScheduleRiskConfig = DEFAULT_CONFIG,
) -> ScheduleRiskAssessment:
    """规则式评估：各规则产出 (level, reason)，取最高等级。"""
    hits: List[Tuple[str, str]] = []

    if continuous_drive_minutes is not None:
        if continuous_drive_minutes >= config.max_continuous_drive_minutes_high:
            hits.append(("HIGH", "连续驾驶 %.0f 分钟，超过高风险阈值 %.0f 分钟"
                         % (continuous_drive_minutes, config.max_continuous_drive_minutes_high)))
        elif continuous_drive_minutes >= config.max_continuous_drive_minutes_attention:
            hits.append(("ATTENTION", "连续驾驶 %.0f 分钟，超过关注阈值 %.0f 分钟"
                         % (continuous_drive_minutes, config.max_continuous_drive_minutes_attention)))

    if rest_minutes is not None and rest_minutes < config.min_rest_minutes:
        hits.append(("ATTENTION", "班前/班间休息仅 %.0f 分钟，低于建议值 %.0f 分钟"
                     % (rest_minutes, config.min_rest_minutes)))

    start_dt = _parse_dt(shift_start)
    end_dt = _parse_dt(shift_end)
    if start_dt is not None and end_dt is not None:
        hours = (end_dt - start_dt).total_seconds() / 3600.0
        if hours >= config.shift_hours_attention:
            hits.append(("ATTENTION", "单班次时长 %.1f 小时，达到关注阈值 %.1f 小时"
                         % (hours, config.shift_hours_attention)))

    if consecutive_work_days is not None:
        if consecutive_work_days >= config.consecutive_work_days_high:
            hits.append(("HIGH", "连续出勤 %d 天，达到高风险阈值 %d 天"
                         % (consecutive_work_days, config.consecutive_work_days_high)))
        elif consecutive_work_days >= config.consecutive_work_days_attention:
            hits.append(("ATTENTION", "连续出勤 %d 天，达到关注阈值 %d 天"
                         % (consecutive_work_days, config.consecutive_work_days_attention)))

    if start_dt is not None and config.night_start_hour_begin <= start_dt.hour < config.night_start_hour_end:
        hits.append(("ATTENTION", "凌晨时段（%d-%d 点）开班，生物钟疲劳风险偏高"
                     % (config.night_start_hour_begin, config.night_start_hour_end)))

    if historical_fatigue_event_count >= config.fatigue_events_high:
        hits.append(("HIGH", "历史疲劳事件 %d 起，达到高风险阈值 %d 起"
                     % (historical_fatigue_event_count, config.fatigue_events_high)))
    elif historical_fatigue_event_count >= config.fatigue_events_attention:
        hits.append(("ATTENTION", "历史疲劳事件 %d 起，达到关注阈值 %d 起"
                     % (historical_fatigue_event_count, config.fatigue_events_attention)))

    if hits:
        level = max(hits, key=lambda h: LEVEL_ORDER[h[0]])[0]
    else:
        level = "NORMAL"
    recommendation = _LEVEL_TO_RECOMMENDATION[level]
    reasons = [r for _, r in hits]

    if level == "NORMAL":
        message = "排班未见明显疲劳风险，保持现有休息安排。"
    elif level == "ATTENTION":
        message = "；".join(reasons) + "。建议关注休息间隔，必要时安排休息。"
    else:
        message = "；".join(reasons) + "。建议调度复核该班次排班与休息安排。"

    return ScheduleRiskAssessment(
        schedule_risk_level=level,
        recommendation=recommendation,
        reasons=reasons,
        message=message,
    )
