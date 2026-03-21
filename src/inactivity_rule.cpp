#include "inactivity_rule.h"

RuleResult EvaluateInactivityRule(const RuleContext& context,
                                  const RuleThresholds& thresholds) {
  if (IsSleepWindow(context.minuteOfDay)) {
    return NoRuleResult();
  }

  if ((context.nowMs - context.lastActivityMs) > thresholds.noActivityWarnMs) {
    return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonInactivity);
  }

  return NoRuleResult();
}
