#include "activity_drop_rule.h"

RuleResult EvaluateActivityDropRule(const RuleContext& context,
                                    const RuleThresholds& thresholds) {
  if (context.minuteOfDay <= thresholds.activityDropCheckMinute ||
      context.activityBaseline == 0) {
    return NoRuleResult();
  }

  if (context.totalTriggersToday < (context.activityBaseline / 2U)) {
    return TriggerRule(AlertLevel::WARNING, AlertProtocol::kReasonActivityDrop);
  }

  return NoRuleResult();
}
