"""TASK H 司机关怀引擎。

定位：安全提示与关怀建议，不作归责。
- 单次疲劳事件 → 仅记录（NORMAL）；
- 重复发生 → 建议休息（REST_RECOMMENDED）；
- 连续多班次高风险 → 建议调度检查排班（SCHEDULE_REVIEW）。

文案风格示例：'近期疲劳风险主要集中在下午班次，建议检查该班次休息间隔。'
阈值集中在 DriverCareConfig，数值均为 **仅工程初始值，需实车标定**。
"""
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Optional

CARE_LEVELS = ("NORMAL", "REST_RECOMMENDED", "SCHEDULE_REVIEW")

# 时段分桶（工程初始划分，用于生成关怀文案的聚焦时段）
PERIOD_BUCKETS = (
    (0, 6, "凌晨班次"),
    (6, 12, "上午班次"),
    (12, 18, "下午班次"),
    (18, 24, "晚间班次"),
)


@dataclass(frozen=True)
class DriverCareConfig:
    """关怀引擎阈值（以下全部为仅工程初始值，需实车标定）。"""
    repeat_event_count_rest: int = 3             # 观察窗内疲劳提示 >= N 次 → 建议休息
    consecutive_high_risk_shifts_review: int = 2  # 连续 >= N 个高风险班次 → 建议调度检查排班
    focus_period_min_count: int = 2              # 某时段事件 >= N 起才在文案中点名时段


DEFAULT_CONFIG = DriverCareConfig()


@dataclass
class CareAdvice:
    care_level: str                 # NORMAL / REST_RECOMMENDED / SCHEDULE_REVIEW
    message: str
    reasons: List[str] = field(default_factory=list)


def _parse_hour(ts: str) -> Optional[int]:
    s = ts.strip()
    candidate = s[:-1] + "+00:00" if s.endswith("Z") else s
    try:
        return datetime.fromisoformat(candidate).hour
    except ValueError:
        return None


def dominant_period(timestamps: List[str], min_count: int = 2) -> Optional[str]:
    """找出事件最集中的时段（如 '下午班次'）；样本不足或无法解析时返回 None。"""
    counts = {name: 0 for _, _, name in PERIOD_BUCKETS}
    for ts in timestamps:
        hour = _parse_hour(ts)
        if hour is None:
            continue
        for begin, end, name in PERIOD_BUCKETS:
            if begin <= hour < end:
                counts[name] += 1
                break
    name = max(counts, key=lambda k: counts[k])
    if counts[name] >= min_count:
        return name
    return None


def assess_driver_care(
    recent_fatigue_event_count: int,
    consecutive_high_risk_shifts: int = 0,
    focus_period: Optional[str] = None,
    config: DriverCareConfig = DEFAULT_CONFIG,
) -> CareAdvice:
    """根据近期疲劳事件与连续高风险班次数给出关怀建议。"""
    if consecutive_high_risk_shifts >= config.consecutive_high_risk_shifts_review:
        reasons = ["连续 %d 个班次疲劳风险偏高" % consecutive_high_risk_shifts]
        if focus_period:
            message = ("近期疲劳风险主要集中在%s，"
                       "建议调度检查该班次休息间隔与排班安排。" % focus_period)
        else:
            message = "近期连续多个班次疲劳风险偏高，建议调度检查排班与休息间隔。"
        return CareAdvice("SCHEDULE_REVIEW", message, reasons)

    if recent_fatigue_event_count >= config.repeat_event_count_rest:
        reasons = ["观察窗内疲劳提示 %d 次" % recent_fatigue_event_count]
        if focus_period:
            message = ("近期疲劳风险主要集中在%s，"
                       "建议检查该班次休息间隔。" % focus_period)
        else:
            message = "近期多次出现疲劳提示，建议合理安排休息、调整作息。"
        return CareAdvice("REST_RECOMMENDED", message, reasons)

    return CareAdvice("NORMAL", "单次疲劳提示已记录，持续关注；不作归责。", [])
