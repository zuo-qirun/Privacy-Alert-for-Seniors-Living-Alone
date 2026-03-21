from dataclasses import dataclass

from .protocol import (
    REASON_ACTIVITY_DROP,
    REASON_DAYTIME_BEDREST,
    REASON_INACTIVE,
    REASON_LATE_WAKEUP,
    REASON_NONE,
    REASON_TOILET_CRITICAL,
    REASON_TOILET_WARNING,
)


@dataclass(frozen=True)
class RuleContext:
    now_ms: int
    minute_of_day: int
    demo_mode: bool
    bed_occupied: bool
    bed_occupied_start_ms: int
    toilet_enter_ms: int
    last_activity_ms: int
    bedroom_pir_triggers_today: int
    total_triggers_today: int
    wakeup_baseline_minute: int
    activity_baseline: int


@dataclass(frozen=True)
class RuleThresholds:
    toilet_warn_ms: int
    toilet_critical_ms: int
    bed_warn_ms: int
    no_activity_warn_ms: int
    demo_wakeup_stay_ms: int
    wakeup_tolerance_minutes: int
    activity_drop_check_minute: int
    wake_bedroom_pir_min_triggers: int


@dataclass(frozen=True)
class RuleResult:
    triggered: bool = False
    level: str = "normal"
    reason_code: str = REASON_NONE


@dataclass(frozen=True)
class RuleEvaluation:
    wake_rule_active: bool = False
    toilet_warning_active: bool = False
    toilet_critical_active: bool = False
    bedrest_active: bool = False
    inactivity_active: bool = False
    activity_drop_active: bool = False
    primary_level: str = "normal"
    primary_reason_code: str = REASON_NONE


def _trigger(level: str, reason_code: str) -> RuleResult:
    return RuleResult(True, level, reason_code)


def is_sleep_window(minute_of_day: int) -> bool:
    return minute_of_day >= 23 * 60 or minute_of_day <= 6 * 60


def is_daytime_window(minute_of_day: int) -> bool:
    return 8 * 60 <= minute_of_day <= 21 * 60


def evaluate_wake_rule(context: RuleContext, thresholds: RuleThresholds) -> RuleResult:
    if not context.bed_occupied or context.bed_occupied_start_ms == 0:
        return RuleResult()
    if context.demo_mode:
        if context.now_ms - context.bed_occupied_start_ms > thresholds.demo_wakeup_stay_ms:
            return _trigger("warning", REASON_LATE_WAKEUP)
        return RuleResult()
    threshold_minute = context.wakeup_baseline_minute + thresholds.wakeup_tolerance_minutes
    if (
        context.minute_of_day > threshold_minute
        and context.bedroom_pir_triggers_today < thresholds.wake_bedroom_pir_min_triggers
    ):
        return _trigger("warning", REASON_LATE_WAKEUP)
    return RuleResult()


def evaluate_toilet_rule(context: RuleContext, thresholds: RuleThresholds) -> RuleResult:
    if context.toilet_enter_ms == 0:
        return RuleResult()
    stay_ms = context.now_ms - context.toilet_enter_ms
    if stay_ms > thresholds.toilet_critical_ms:
        return _trigger("critical", REASON_TOILET_CRITICAL)
    if stay_ms > thresholds.toilet_warn_ms:
        return _trigger("warning", REASON_TOILET_WARNING)
    return RuleResult()


def evaluate_bedrest_rule(context: RuleContext, thresholds: RuleThresholds) -> RuleResult:
    if not is_daytime_window(context.minute_of_day):
        return RuleResult()
    if not context.bed_occupied or context.bed_occupied_start_ms == 0:
        return RuleResult()
    if context.now_ms - context.bed_occupied_start_ms > thresholds.bed_warn_ms:
        return _trigger("warning", REASON_DAYTIME_BEDREST)
    return RuleResult()


def evaluate_inactivity_rule(context: RuleContext, thresholds: RuleThresholds) -> RuleResult:
    if is_sleep_window(context.minute_of_day):
        return RuleResult()
    if context.now_ms - context.last_activity_ms > thresholds.no_activity_warn_ms:
        return _trigger("warning", REASON_INACTIVE)
    return RuleResult()


def evaluate_activity_drop_rule(context: RuleContext, thresholds: RuleThresholds) -> RuleResult:
    if context.minute_of_day <= thresholds.activity_drop_check_minute:
        return RuleResult()
    if context.activity_baseline == 0:
        return RuleResult()
    if context.total_triggers_today < context.activity_baseline / 2:
        return _trigger("warning", REASON_ACTIVITY_DROP)
    return RuleResult()


def evaluate_rule_set(context: RuleContext, thresholds: RuleThresholds) -> RuleEvaluation:
    toilet = evaluate_toilet_rule(context, thresholds)
    wake = evaluate_wake_rule(context, thresholds)
    bedrest = evaluate_bedrest_rule(context, thresholds)
    inactivity = evaluate_inactivity_rule(context, thresholds)
    activity_drop = evaluate_activity_drop_rule(context, thresholds)

    results = [toilet, wake, bedrest, inactivity, activity_drop]
    primary = RuleResult()
    order = {"normal": 0, "warning": 1, "critical": 2}
    for result in results:
        if not result.triggered:
            continue
        if order[result.level] > order[primary.level]:
            primary = result
        elif primary.level == "normal":
            primary = result

    return RuleEvaluation(
        wake_rule_active=wake.triggered,
        toilet_warning_active=toilet.triggered and toilet.level == "warning",
        toilet_critical_active=toilet.triggered and toilet.level == "critical",
        bedrest_active=bedrest.triggered,
        inactivity_active=inactivity.triggered,
        activity_drop_active=activity_drop.triggered,
        primary_level=primary.level,
        primary_reason_code=primary.reason_code,
    )
