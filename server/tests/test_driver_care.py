"""TASK H 司机关怀引擎分支测试（全部 mock 输入）。"""
from app.driver_care import (
    DEFAULT_CONFIG,
    assess_driver_care,
    dominant_period,
)


def test_single_event_only_record():
    r = assess_driver_care(recent_fatigue_event_count=1)
    assert r.care_level == "NORMAL"
    assert "已记录" in r.message
    assert r.reasons == []


def test_repeated_events_rest_recommended():
    n = DEFAULT_CONFIG.repeat_event_count_rest
    r = assess_driver_care(recent_fatigue_event_count=n)
    assert r.care_level == "REST_RECOMMENDED"
    assert "休息" in r.message
    assert r.reasons


def test_consecutive_high_risk_shifts_schedule_review():
    n = DEFAULT_CONFIG.consecutive_high_risk_shifts_review
    r = assess_driver_care(recent_fatigue_event_count=0,
                           consecutive_high_risk_shifts=n)
    assert r.care_level == "SCHEDULE_REVIEW"
    assert "排班" in r.message


def test_schedule_review_overrides_rest():
    r = assess_driver_care(recent_fatigue_event_count=99,
                           consecutive_high_risk_shifts=2)
    assert r.care_level == "SCHEDULE_REVIEW"


def test_focus_period_message_style():
    r = assess_driver_care(recent_fatigue_event_count=5,
                           focus_period="下午班次")
    assert r.care_level == "REST_RECOMMENDED"
    assert r.message == "近期疲劳风险主要集中在下午班次，建议检查该班次休息间隔。"
    r2 = assess_driver_care(recent_fatigue_event_count=0,
                            consecutive_high_risk_shifts=3,
                            focus_period="下午班次")
    assert r2.care_level == "SCHEDULE_REVIEW"
    assert "下午班次" in r2.message


def test_dominant_period():
    ts = ["2026-08-24T13:00:00", "2026-08-24T14:20:00", "2026-08-24T09:00:00"]
    assert dominant_period(ts) == "下午班次"
    # 样本不足 → None
    assert dominant_period(["2026-08-24T13:00:00"]) is None
    # 无法解析 → None
    assert dominant_period(["bad", None or ""]) is None
    assert dominant_period([]) is None


def test_care_messages_never_blame_driver():
    """关怀文案红线：任何分支都不得出现归责/处罚类措辞。"""
    banned = ("罚", "扣", "penalty", "fine", "punish", "deduct")
    cases = [
        assess_driver_care(0),
        assess_driver_care(10),
        assess_driver_care(0, consecutive_high_risk_shifts=5),
        assess_driver_care(10, consecutive_high_risk_shifts=5, focus_period="凌晨班次"),
    ]
    for advice in cases:
        text = advice.message + " " + " ".join(advice.reasons)
        for word in banned:
            assert word not in text
