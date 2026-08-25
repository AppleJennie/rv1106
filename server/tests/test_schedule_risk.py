"""TASK G 排班风险规则引擎分支测试（全部 mock 输入）。"""
from app.schedule_risk import DEFAULT_CONFIG, evaluate_shift


def _base_kwargs():
    return {
        "continuous_drive_minutes": 120.0,
        "rest_minutes": 60.0,
        "shift_start": "08:00",
        "shift_end": "16:00",
        "consecutive_work_days": 2,
        "historical_fatigue_event_count": 0,
    }


def test_normal_shift():
    r = evaluate_shift(**_base_kwargs())
    assert r.schedule_risk_level == "NORMAL"
    assert r.recommendation == "NORMAL"
    assert r.reasons == []
    assert r.message


def test_continuous_drive_attention():
    kw = _base_kwargs()
    kw["continuous_drive_minutes"] = 250.0  # >= 240
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "ATTENTION"
    assert r.recommendation == "REST_RECOMMENDED"
    assert any("连续驾驶" in x for x in r.reasons)


def test_continuous_drive_high():
    kw = _base_kwargs()
    kw["continuous_drive_minutes"] = 400.0  # >= 360
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "HIGH"
    assert r.recommendation == "SCHEDULE_REVIEW"


def test_insufficient_rest():
    kw = _base_kwargs()
    kw["rest_minutes"] = 10.0  # < 20
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "ATTENTION"
    assert any("休息" in x for x in r.reasons)


def test_long_shift_duration():
    kw = _base_kwargs()
    kw["shift_start"] = "06:00"
    kw["shift_end"] = "20:00"  # 14h >= 10h
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "ATTENTION"
    assert any("班次时长" in x for x in r.reasons)


def test_consecutive_work_days():
    kw = _base_kwargs()
    kw["consecutive_work_days"] = 5
    assert evaluate_shift(**kw).schedule_risk_level == "ATTENTION"
    kw["consecutive_work_days"] = 7
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "HIGH"
    assert r.recommendation == "SCHEDULE_REVIEW"


def test_night_start():
    kw = _base_kwargs()
    kw["shift_start"] = "2026-08-24T03:30:00"
    kw["shift_end"] = "2026-08-24T09:30:00"
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "ATTENTION"
    assert any("凌晨" in x for x in r.reasons)


def test_historical_fatigue_count():
    kw = _base_kwargs()
    kw["historical_fatigue_event_count"] = 3
    assert evaluate_shift(**kw).schedule_risk_level == "ATTENTION"
    kw["historical_fatigue_event_count"] = 6
    assert evaluate_shift(**kw).schedule_risk_level == "HIGH"


def test_max_level_wins_and_message_style():
    kw = _base_kwargs()
    kw["continuous_drive_minutes"] = 400.0  # HIGH
    kw["rest_minutes"] = 5.0                # ATTENTION
    kw["historical_fatigue_event_count"] = 4  # ATTENTION
    r = evaluate_shift(**kw)
    assert r.schedule_risk_level == "HIGH"
    assert r.recommendation == "SCHEDULE_REVIEW"
    assert len(r.reasons) == 3
    assert "建议调度复核" in r.message


def test_none_inputs_no_crash():
    r = evaluate_shift()
    assert r.schedule_risk_level == "NORMAL"
    # 无法解析的时间串被忽略而不是报错
    r2 = evaluate_shift(shift_start="bad", shift_end=None,
                        historical_fatigue_event_count=0)
    assert r2.schedule_risk_level == "NORMAL"


def test_threshold_boundaries():
    cfg = DEFAULT_CONFIG
    kw = _base_kwargs()
    kw["continuous_drive_minutes"] = cfg.max_continuous_drive_minutes_attention
    assert evaluate_shift(**kw).schedule_risk_level == "ATTENTION"
    kw["continuous_drive_minutes"] = cfg.max_continuous_drive_minutes_attention - 1
    assert evaluate_shift(**kw).schedule_risk_level == "NORMAL"
    kw["rest_minutes"] = cfg.min_rest_minutes  # 等于阈值不触发
    kw["continuous_drive_minutes"] = 100.0
    assert evaluate_shift(**kw).schedule_risk_level == "NORMAL"
