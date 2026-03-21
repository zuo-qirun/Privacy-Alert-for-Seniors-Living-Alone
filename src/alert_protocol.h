#pragma once

#include <Arduino.h>
#include <cstring>

enum class AlertLevel : uint8_t {
  INFO = 0,
  WARNING = 1,
  CRITICAL = 2,
};

namespace AlertProtocol {

constexpr char kMessageTypeAlert[] = "alert";
constexpr char kMessageTypeStatus[] = "status";
constexpr char kMessageTypeSensorRealtime[] = "sensor_realtime";

constexpr char kRiskNormal[] = "normal";
constexpr char kRiskWarning[] = "warning";
constexpr char kRiskCritical[] = "critical";

constexpr char kReasonNone[] = "none";
constexpr char kReasonLateWakeup[] = "late_wakeup";
constexpr char kReasonToiletStayWarning[] = "toilet_stay_12m";
constexpr char kReasonToiletStayCritical[] = "toilet_stay_18m";
constexpr char kReasonDaytimeBedrest[] = "daytime_bedrest_3h";
constexpr char kReasonInactivity[] = "inactive_2h";
constexpr char kReasonActivityDrop[] = "activity_drop_50pct";

inline const char* alertLevelToString(AlertLevel level) {
  switch (level) {
    case AlertLevel::INFO:
      return kRiskNormal;
    case AlertLevel::WARNING:
      return kRiskWarning;
    case AlertLevel::CRITICAL:
      return kRiskCritical;
    default:
      return "unknown";
  }
}

inline const char* alertLevelToChinese(AlertLevel level) {
  switch (level) {
    case AlertLevel::INFO:
      return "正常";
    case AlertLevel::WARNING:
      return "中度异常";
    case AlertLevel::CRITICAL:
      return "高风险";
    default:
      return "未知";
  }
}

inline bool reasonEquals(const char* lhs, const char* rhs) {
  return lhs != nullptr && rhs != nullptr && std::strcmp(lhs, rhs) == 0;
}

inline const char* reasonTitle(const char* reasonCode) {
  if (reasonEquals(reasonCode, kReasonLateWakeup)) {
    return "Wakeup Anomaly";
  }
  if (reasonEquals(reasonCode, kReasonToiletStayWarning)) {
    return "Toilet Stay Warning";
  }
  if (reasonEquals(reasonCode, kReasonToiletStayCritical)) {
    return "Toilet Stay Critical";
  }
  if (reasonEquals(reasonCode, kReasonDaytimeBedrest)) {
    return "Long Bed Occupancy";
  }
  if (reasonEquals(reasonCode, kReasonInactivity)) {
    return "No Activity Warning";
  }
  if (reasonEquals(reasonCode, kReasonActivityDrop)) {
    return "Activity Drop Warning";
  }
  return "Status Update";
}

inline const char* reasonDetail(const char* reasonCode) {
  if (reasonEquals(reasonCode, kReasonLateWakeup)) {
    return "Wakeup later than baseline threshold.";
  }
  if (reasonEquals(reasonCode, kReasonToiletStayWarning)) {
    return "Toilet stay exceeded the warning threshold.";
  }
  if (reasonEquals(reasonCode, kReasonToiletStayCritical)) {
    return "Toilet stay exceeded the critical threshold.";
  }
  if (reasonEquals(reasonCode, kReasonDaytimeBedrest)) {
    return "Daytime bed occupancy exceeded the threshold.";
  }
  if (reasonEquals(reasonCode, kReasonInactivity)) {
    return "No obvious activity exceeded the threshold.";
  }
  if (reasonEquals(reasonCode, kReasonActivityDrop)) {
    return "Today's activity dropped below the rolling baseline.";
  }
  return "No active reason.";
}

}  // namespace AlertProtocol
