#include "bedrest_rule.h"

RuleResult EvaluateBedrestRule(const RuleContext& context, const RuleThresholds& thresholds) {
  if (!IsDaytimeWindow(context.minuteOfDay) || !context.bedOccupied ||
      context.bedOccupiedStartMs == 0) {
    return NoRuleResult();
  }

  if ((context.nowMs - context.bedOccupiedStartMs) > thresholds.bedWarnMs) {
    return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonDaytimeBedrest);
  }

  return NoRuleResult();
}
