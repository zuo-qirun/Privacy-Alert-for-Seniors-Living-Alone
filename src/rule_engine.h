#pragma once

#include "rule_types.h"

struct RuleEvaluation {
  bool wakeRuleActive = false;
  bool toiletWarningActive = false;
  bool toiletCriticalActive = false;
  bool bedrestActive = false;
  bool inactivityActive = false;
  bool activityDropActive = false;
  AlertLevel primaryLevel = AlertLevel::INFO;
  const char* primaryReasonCode = AlertProtocol::kReasonNone;
};

RuleEvaluation EvaluateRuleSet(const RuleContext& context, const RuleThresholds& thresholds);
