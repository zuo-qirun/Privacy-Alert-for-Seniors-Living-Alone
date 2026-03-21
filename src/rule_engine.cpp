#include "rule_engine.h"

#include "activity_drop_rule.h"
#include "bedrest_rule.h"
#include "inactivity_rule.h"
#include "toilet_rule.h"
#include "wake_rule.h"

namespace {

void ApplyPrimaryRule(RuleEvaluation& evaluation, const RuleResult& result) {
  if (!result.triggered) {
    return;
  }

  if (static_cast<uint8_t>(result.level) >
      static_cast<uint8_t>(evaluation.primaryLevel)) {
    evaluation.primaryLevel = result.level;
    evaluation.primaryReasonCode = result.reasonCode;
    return;
  }

  if (evaluation.primaryLevel == AlertLevel::INFO) {
    evaluation.primaryLevel = result.level;
    evaluation.primaryReasonCode = result.reasonCode;
  }
}

}  // namespace

RuleEvaluation EvaluateRuleSet(const RuleContext& context, const RuleThresholds& thresholds) {
  RuleEvaluation evaluation;

  const RuleResult toilet = EvaluateToiletRule(context, thresholds);
  const RuleResult wake = EvaluateWakeRule(context, thresholds);
  const RuleResult bedrest = EvaluateBedrestRule(context, thresholds);
  const RuleResult inactivity = EvaluateInactivityRule(context, thresholds);
  const RuleResult activityDrop = EvaluateActivityDropRule(context, thresholds);

  evaluation.wakeRuleActive = wake.triggered;
  evaluation.toiletWarningActive = toilet.triggered &&
                                   toilet.level == AlertLevel::WARNING;
  evaluation.toiletCriticalActive = toilet.triggered &&
                                    toilet.level == AlertLevel::CRITICAL;
  evaluation.bedrestActive = bedrest.triggered;
  evaluation.inactivityActive = inactivity.triggered;
  evaluation.activityDropActive = activityDrop.triggered;

  ApplyPrimaryRule(evaluation, toilet);
  ApplyPrimaryRule(evaluation, wake);
  ApplyPrimaryRule(evaluation, bedrest);
  ApplyPrimaryRule(evaluation, inactivity);
  ApplyPrimaryRule(evaluation, activityDrop);

  return evaluation;
}
