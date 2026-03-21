#include "toilet_rule.h"

RuleResult EvaluateToiletRule(const RuleContext& context, const RuleThresholds& thresholds) {
  if (context.toiletEnterMs == 0) {
    return NoRuleResult();
  }

  const uint32_t stayMs = context.nowMs - context.toiletEnterMs;
  if (stayMs > thresholds.toiletCriticalMs) {
    return TriggerRule(AlertLevel::CRITICAL, AlertProtocol::kReasonToiletStayCritical);
  }
  if (stayMs > thresholds.toiletWarnMs) {
    return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonToiletStayWarning);
  }

  return NoRuleResult();
}
