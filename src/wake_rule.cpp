#include "wake_rule.h"

#include "config_rules.h"

RuleResult EvaluateWakeRule(const RuleContext& context, const RuleThresholds& thresholds) {
  if (!context.bedOccupied || context.bedOccupiedStartMs == 0) {
    return NoRuleResult();
  }

  if (context.demoMode) {
    if ((context.nowMs - context.bedOccupiedStartMs) > thresholds.demoWakeupStayMs) {
      return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonLateWakeup);
    }
    return NoRuleResult();
  }

  const uint16_t thresholdMinute = static_cast<uint16_t>(
      context.wakeupBaselineMinute + thresholds.wakeupToleranceMinutes);
  if (context.minuteOfDay > thresholdMinute &&
      context.bedroomPirTriggersToday < thresholds.wakeBedroomPirMinTriggers) {
    return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonLateWakeup);
  }

  return NoRuleResult();
}
