#pragma once

#include <Arduino.h>

namespace ConfigRules {

constexpr uint32_t kMinuteMs = 60000UL;
constexpr uint32_t kHourMs = 3600000UL;
constexpr uint32_t kDayMs = 86400000UL;

constexpr uint32_t kToiletWarnMs = 12UL * kMinuteMs;
constexpr uint32_t kToiletCriticalMs = 18UL * kMinuteMs;
constexpr uint32_t kBedWarnMs = 3UL * kHourMs;
constexpr uint32_t kNoActivityWarnMs = 2UL * kHourMs;

constexpr uint32_t kDemoWarnMs = 10000UL;
constexpr uint32_t kDemoWakeupStayMs = 10000UL;
constexpr uint32_t kDemoToiletWarnMs = 1000UL;

constexpr uint16_t kDefaultWakeupMinute = 7 * 60;
constexpr uint16_t kWakeupToleranceMinutes = 90;
constexpr uint8_t kHistoryDays = 5;
constexpr uint16_t kInvalidWakeupMinute = 0xFFFF;

constexpr uint16_t kActivityDropCheckMinute = 20 * 60;
constexpr uint8_t kWakeBedroomPirMinTriggers = 2;

}  // namespace ConfigRules
