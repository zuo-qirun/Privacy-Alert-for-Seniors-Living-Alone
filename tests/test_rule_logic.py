from shared_protocol.protocol import (
    REASON_ACTIVITY_DROP,
    REASON_DAYTIME_BEDREST,
    REASON_INACTIVE,
    REASON_LATE_WAKEUP,
    REASON_TOILET_CRITICAL,
    REASON_TOILET_WARNING,
)
from shared_protocol.rule_logic import (
    RuleContext,
    RuleThresholds,
    evaluate_activity_drop_rule,
    evaluate_bedrest_rule,
    evaluate_inactivity_rule,
    evaluate_rule_set,
    evaluate_toilet_rule,
    evaluate_wake_rule,
)


def default_thresholds() -> RuleThresholds:
    return RuleThresholds(
        toilet_warn_ms=12 * 60 * 1000,
        toilet_critical_ms=18 * 60 * 1000,
        bed_warn_ms=3 * 60 * 60 * 1000,
        no_activity_warn_ms=2 * 60 * 60 * 1000,
        demo_wakeup_stay_ms=10 * 1000,
        wakeup_tolerance_minutes=90,
        activity_drop_check_minute=20 * 60,
        wake_bedroom_pir_min_triggers=2,
    )


def base_context() -> RuleContext:
    return RuleContext(
        now_ms=0,
        minute_of_day=8 * 60,
        demo_mode=False,
        bed_occupied=False,
        bed_occupied_start_ms=0,
        toilet_enter_ms=0,
        last_activity_ms=0,
        bedroom_pir_triggers_today=3,
        total_triggers_today=100,
        wakeup_baseline_minute=7 * 60,
        activity_baseline=120,
    )


def test_wake_rule_triggers_after_baseline_tolerance():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "minute_of_day": 9 * 60 + 31, "bed_occupied": True, "bed_occupied_start_ms": 1, "bedroom_pir_triggers_today": 1})
    result = evaluate_wake_rule(ctx, default_thresholds())
    assert result.triggered is True
    assert result.reason_code == REASON_LATE_WAKEUP


def test_toilet_rule_escalates_to_critical():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "now_ms": 19 * 60 * 1000, "toilet_enter_ms": 1})
    result = evaluate_toilet_rule(ctx, default_thresholds())
    assert result.triggered is True
    assert result.level == "critical"
    assert result.reason_code == REASON_TOILET_CRITICAL


def test_bedrest_rule_requires_daytime_and_long_bed_occupancy():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "minute_of_day": 10 * 60, "now_ms": 4 * 60 * 60 * 1000, "bed_occupied": True, "bed_occupied_start_ms": 1})
    result = evaluate_bedrest_rule(ctx, default_thresholds())
    assert result.triggered is True
    assert result.reason_code == REASON_DAYTIME_BEDREST


def test_inactivity_rule_ignores_sleep_window():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "minute_of_day": 23 * 60 + 5, "now_ms": 5 * 60 * 60 * 1000, "last_activity_ms": 0})
    result = evaluate_inactivity_rule(ctx, default_thresholds())
    assert result.triggered is False


def test_activity_drop_rule_triggers_after_evening_cutoff():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "minute_of_day": 20 * 60 + 1, "total_triggers_today": 40, "activity_baseline": 120})
    result = evaluate_activity_drop_rule(ctx, default_thresholds())
    assert result.triggered is True
    assert result.reason_code == REASON_ACTIVITY_DROP


def test_rule_engine_prefers_critical_reason():
    ctx = base_context()
    ctx = RuleContext(
        now_ms=19 * 60 * 1000,
        minute_of_day=10 * 60,
        demo_mode=False,
        bed_occupied=True,
        bed_occupied_start_ms=1,
        toilet_enter_ms=1,
        last_activity_ms=0,
        bedroom_pir_triggers_today=0,
        total_triggers_today=20,
        wakeup_baseline_minute=7 * 60,
        activity_baseline=120,
    )
    evaluation = evaluate_rule_set(ctx, default_thresholds())
    assert evaluation.primary_level == "critical"
    assert evaluation.primary_reason_code == REASON_TOILET_CRITICAL
    assert evaluation.wake_rule_active is True
    assert evaluation.toilet_critical_active is True


def test_toilet_warning_exists_before_critical():
    ctx = base_context()
    ctx = RuleContext(**{**ctx.__dict__, "now_ms": 13 * 60 * 1000, "toilet_enter_ms": 1})
    result = evaluate_toilet_rule(ctx, default_thresholds())
    assert result.triggered is True
    assert result.reason_code == REASON_TOILET_WARNING
