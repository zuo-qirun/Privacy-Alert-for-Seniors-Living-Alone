#pragma once

#include "rule_types.h"

RuleResult EvaluateActivityDropRule(const RuleContext& context,
                                    const RuleThresholds& thresholds);
