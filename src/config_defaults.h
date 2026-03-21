#pragma once

#include <Arduino.h>
#include <time.h>

namespace ConfigDefaults {

constexpr char kApSsid[] = "SeniorAlert-Config";
constexpr char kApPassword[] = "12345678";
constexpr char kMqttBroker[] = "bemfa.com";
constexpr uint16_t kMqttPort = 9501;
constexpr char kMqttTopic[] = "senioralertevents";

constexpr char kConfigPath[] = "/config.json";

constexpr uint32_t kMqttHeartbeatMs = 30000UL;
constexpr uint32_t kMqttSensorPublishMs = 1000UL;
constexpr uint8_t kWifiFailToApThreshold = 3;

constexpr byte kDnsPort = 53;
constexpr time_t kMinValidEpoch = 1704067200;

}  // namespace ConfigDefaults
