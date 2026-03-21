#pragma once

#include <Arduino.h>

#include "alert_protocol.h"

struct RuleContext {
  uint32_t nowMs = 0;
  uint16_t minuteOfDay = 0;
  bool demoMode = false;
  bool bedOccupied = false;
  uint32_t bedOccupiedStartMs = 0;
  uint32_t toiletEnterMs = 0;
  uint32_t lastActivityMs = 0;
  uint16_t bedroomPirTriggersToday = 0;
  uint16_t totalTriggersToday = 0;
  uint16_t wakeupBaselineMinute = 0;
  uint16_t activityBaseline = 0;
};

struct RuleThresholds {
  uint32_t toiletWarnMs = 0;
  uint32_t toiletCriticalMs = 0;
  uint32_t bedWarnMs = 0;
  uint32_t noActivityWarnMs = 0;
  uint32_t demoWakeupStayMs = 0;
  uint16_t wakeupToleranceMinutes = 0;
  uint16_t activityDropCheckMinute = 0;
  uint8_t wakeBedroomPirMinTriggers = 0;
};

struct RuleResult {
  bool triggered = false;
  AlertLevel level = AlertLevel::INFO;
  const char* reasonCode = AlertProtocol::kReasonNone;
};

inline RuleResult NoRuleResult() {
  return {};
}

inline RuleResult TriggerRule(AlertLevel level, const char* reasonCode) {
  RuleResult result;
  result.triggered = true;
  result.level = level;
  result.reasonCode = reasonCode;
  return result;
}

inline bool IsSleepWindow(uint16_t minuteOfDay) {
  return (minuteOfDay >= 23 * 60) || (minuteOfDay <= 6 * 60);
}

inline bool IsDaytimeWindow(uint16_t minuteOfDay) {
  return minuteOfDay >= 8 * 60 && minuteOfDay <= 21 * 60;
}
