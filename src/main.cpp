#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>

#include "alert_protocol.h"
#include "config_defaults.h"
#include "config_pins.h"
#include "config_rules.h"
#include "rule_engine.h"
#include "secrets.h"

struct DayRecord {
  uint16_t totalTriggers = 0;
  uint16_t wakeupMinute = ConfigRules::kInvalidWakeupMinute;
};

struct AppConfig {
  String ssid = "";
  String pass = "";
  bool demoMode = false;
  bool bedActiveLow = true;
  String mqttBroker = ConfigDefaults::kMqttBroker;
  uint16_t mqttPort = ConfigDefaults::kMqttPort;
  String mqttTopic = ConfigDefaults::kMqttTopic;
};

struct TimeContext {
  bool hasRealTime = false;
  uint16_t minuteOfDay = 0;
  uint32_t dayKey = 0;
};

struct RuntimeState {
  bool bedroomPir = false;
  bool toiletPir = false;
  bool toiletDoorClosed = false;
  bool bedOccupied = false;
  bool lastBedroomPir = false;
  bool lastToiletPir = false;
  bool lastToiletDoorClosed = false;
  bool lastBedOccupied = false;
  uint32_t lastActivityMs = 0;
  uint32_t bedOccupiedStartMs = 0;
  uint32_t toiletEnterMs = 0;
  uint32_t lastMqttRetryMs = 0;
  uint32_t lastMqttHeartbeatMs = 0;
  uint32_t lastMqttSensorPublishMs = 0;
  bool sensorStateChanged = false;
  uint32_t dayKey = 0;
  bool dayKeyHasRealTime = false;
  uint16_t bedroomPirTriggersToday = 0;
  uint16_t toiletDoorChangesToday = 0;
  uint16_t totalTriggersToday = 0;
  uint16_t wakeupMinuteToday = ConfigRules::kInvalidWakeupMinute;
  bool wakeupWarnSent = false;
  bool toiletWarnSent = false;
  bool toiletCriticalSent = false;
  bool bedWarnSent = false;
  bool noActivityWarnSent = false;
  bool activityDropWarnSent = false;
  AlertLevel riskToday = AlertLevel::INFO;
  const char* primaryReasonCode = AlertProtocol::kReasonNone;
  const char* lastAlertReasonCode = AlertProtocol::kReasonNone;
  RuleEvaluation activeEvaluation;
  DayRecord history[ConfigRules::kHistoryDays];
  uint8_t historyWriteIndex = 0;
  uint8_t historyCount = 0;
};

WiFiClient g_wifiClient;
PubSubClient g_mqtt(g_wifiClient);
WebServer g_web(80);
DNSServer g_dns;
RuntimeState g_state;
AppConfig g_config;

bool g_wifiStackReady = false;
bool g_apConfigMode = false;
bool g_timeSynced = false;
bool g_timeSyncRequested = false;
bool g_dnsRunning = false;
uint8_t g_wifiConnectFailCount = 0;
uint32_t g_lastWifiBeginMs = 0;
uint32_t g_lastTimeSyncTryMs = 0;
uint32_t g_lastSerialInputMs = 0;
uint32_t g_lastAdc2WarnMs = 0;
String g_serialLine;

bool tryGetLocalTimeInfo(tm& now);
void refreshEvaluation(uint32_t nowMs);
void handleApiStatus();
void handleApiWifiConfig();
void handleApiDevConfig();
void handleApiDevRiskDrop();
String htmlPage();

void applyPinModes() {
  pinMode(ConfigPins::kPirBedroom, INPUT);
  pinMode(ConfigPins::kPirToilet, INPUT);
  pinMode(ConfigPins::kDoorToilet, INPUT_PULLUP);
  pinMode(
      ConfigPins::kBedPressure,
      ConfigPins::kBedPressureUsePullup ? INPUT_PULLUP : INPUT);
  pinMode(ConfigPins::kBuzzer, OUTPUT);
  digitalWrite(ConfigPins::kBuzzer, LOW);
}

uint16_t minuteOfDayFromUptime(uint32_t nowMs) {
  return static_cast<uint16_t>((nowMs / ConfigRules::kMinuteMs) % (24UL * 60UL));
}

bool isAdc2Pin(int pin) {
  switch (pin) {
    case 0:
    case 2:
    case 4:
    case 12:
    case 13:
    case 14:
    case 15:
    case 25:
    case 26:
    case 27:
      return true;
    default:
      return false;
  }
}

int safeAnalogRead(int pin, bool verbose = true) {
  const bool wifiActive = (WiFi.getMode() != WIFI_OFF);
  if (wifiActive && isAdc2Pin(pin)) {
    if (verbose) {
      const uint32_t now = millis();
      if (now - g_lastAdc2WarnMs > 2000) {
        g_lastAdc2WarnMs = now;
        Serial.printf(
            "[WARN] analogRead(GPIO%d) skipped: ADC2 unavailable while Wi-Fi is active.\n",
            pin);
      }
    }
    return -1;
  }
  return analogRead(pin);
}

bool isManagedSensorOrActuatorPin(int pin) {
  return pin == ConfigPins::kPirBedroom ||
         pin == ConfigPins::kPirToilet ||
         pin == ConfigPins::kDoorToilet ||
         pin == ConfigPins::kBedPressure ||
         pin == ConfigPins::kBuzzer;
}

String formatNowTime() {
  tm now{};
  if (!tryGetLocalTimeInfo(now)) {
    return "N/A";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now);
  return String(buf);
}

bool tryGetLocalTimeInfo(tm& now) {
  if (!g_timeSynced) {
    return false;
  }
  const time_t epoch = time(nullptr);
  if (epoch < ConfigDefaults::kMinValidEpoch) {
    return false;
  }
  return localtime_r(&epoch, &now) != nullptr;
}

TimeContext buildTimeContext(uint32_t nowMs) {
  tm now{};
  if (tryGetLocalTimeInfo(now)) {
    TimeContext ctx;
    ctx.hasRealTime = true;
    ctx.minuteOfDay = static_cast<uint16_t>(now.tm_hour * 60 + now.tm_min);
    ctx.dayKey = static_cast<uint32_t>((now.tm_year + 1900) * 1000 + now.tm_yday);
    return ctx;
  }

  TimeContext ctx;
  ctx.minuteOfDay = minuteOfDayFromUptime(nowMs);
  ctx.dayKey = nowMs / ConfigRules::kDayMs;
  return ctx;
}

bool saveConfig() {
  JsonDocument doc;
  doc["config_version"] = 5;
  doc["ssid"] = g_config.ssid;
  doc["pass"] = g_config.pass;
  doc["demo_mode"] = g_config.demoMode;
  doc["bed_active_low"] = g_config.bedActiveLow;
  doc["mqtt_broker"] = g_config.mqttBroker;
  doc["mqtt_port"] = g_config.mqttPort;
  doc["mqtt_topic"] = g_config.mqttTopic;

  File file = SPIFFS.open(ConfigDefaults::kConfigPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  serializeJson(doc, file);
  file.close();
  return true;
}

void loadConfig() {
  if (!SPIFFS.exists(ConfigDefaults::kConfigPath)) {
    return;
  }

  File file = SPIFFS.open(ConfigDefaults::kConfigPath, FILE_READ);
  if (!file) {
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, file)) {
    file.close();
    return;
  }
  file.close();

  g_config.ssid = doc["ssid"] | "";
  g_config.pass = doc["pass"] | "";
  g_config.demoMode = doc["demo_mode"] | false;
  g_config.bedActiveLow = doc["bed_active_low"] | true;
  g_config.mqttBroker = doc["mqtt_broker"] | ConfigDefaults::kMqttBroker;
  g_config.mqttPort = doc["mqtt_port"] | ConfigDefaults::kMqttPort;
  g_config.mqttTopic = doc["mqtt_topic"] | ConfigDefaults::kMqttTopic;

  g_config.mqttBroker.trim();
  g_config.mqttTopic.trim();
  if (g_config.mqttBroker.length() == 0) {
    g_config.mqttBroker = ConfigDefaults::kMqttBroker;
  }
  if (g_config.mqttPort == 0) {
    g_config.mqttPort = ConfigDefaults::kMqttPort;
  }
  if (g_config.mqttTopic.length() == 0) {
    g_config.mqttTopic = ConfigDefaults::kMqttTopic;
  }
}

void requestTimeSync() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  configTime(8 * 3600, 0, "ntp1.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  g_timeSyncRequested = true;
  g_lastTimeSyncTryMs = millis();
}

void updateTimeSyncStatus(uint32_t nowMs) {
  tm now{};
  if (tryGetLocalTimeInfo(now) || getLocalTime(&now, 10)) {
    g_timeSynced = true;
  }
  if (WiFi.status() == WL_CONNECTED &&
      (!g_timeSyncRequested || (nowMs - g_lastTimeSyncTryMs > 30000))) {
    requestTimeSync();
  }
}

void ensureWifiStackReady() {
  if (g_wifiStackReady) {
    return;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  g_wifiStackReady = true;
}

void startApConfigMode() {
  if (g_apConfigMode) {
    return;
  }
  WiFi.mode(WIFI_AP);
  g_apConfigMode = WiFi.softAP(ConfigDefaults::kApSsid, ConfigDefaults::kApPassword);
  if (g_apConfigMode) {
    g_dns.start(ConfigDefaults::kDnsPort, "*", WiFi.softAPIP());
    g_dnsRunning = true;
    Serial.printf(
        "[INFO] AP provisioning started: %s %s\n",
        ConfigDefaults::kApSsid,
        WiFi.softAPIP().toString().c_str());
  }
}

void stopApConfigMode() {
  if (!g_apConfigMode) {
    return;
  }
  if (g_dnsRunning) {
    g_dns.stop();
    g_dnsRunning = false;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_apConfigMode = false;
}

void ensureWifiConnected() {
  ensureWifiStackReady();
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiConnectFailCount = 0;
    stopApConfigMode();
    return;
  }
  if (g_config.ssid.length() == 0) {
    startApConfigMode();
    return;
  }
  if (g_apConfigMode || millis() - g_lastWifiBeginMs < 5000) {
    return;
  }

  g_lastWifiBeginMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_config.ssid.c_str(), g_config.pass.c_str());

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 10) {
    delay(300);
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_wifiConnectFailCount = 0;
    stopApConfigMode();
    requestTimeSync();
  } else {
    if (g_wifiConnectFailCount < 255) {
      g_wifiConnectFailCount++;
    }
    if (g_wifiConnectFailCount >= ConfigDefaults::kWifiFailToApThreshold) {
      startApConfigMode();
    }
  }
}

bool hasMqttCredentials() {
  return std::strlen(MQTT_PRIVATE_KEY) > 0 ||
         (std::strlen(MQTT_APP_ID) > 0 && std::strlen(MQTT_SECRET_KEY) > 0);
}

void ensureMqttConnected(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED || g_mqtt.connected()) {
    return;
  }
  if (g_config.mqttBroker.length() == 0 || g_config.mqttTopic.length() == 0 ||
      !hasMqttCredentials() || (nowMs - g_state.lastMqttRetryMs < 5000)) {
    return;
  }
  g_state.lastMqttRetryMs = nowMs;
  g_mqtt.setServer(g_config.mqttBroker.c_str(), g_config.mqttPort);

  bool ok = false;
  if (std::strlen(MQTT_PRIVATE_KEY) > 0) {
    ok = g_mqtt.connect(MQTT_PRIVATE_KEY);
  }
  if (!ok && std::strlen(MQTT_APP_ID) > 0 && std::strlen(MQTT_SECRET_KEY) > 0) {
    ok = g_mqtt.connect("senior-alert-app-auth", MQTT_APP_ID, MQTT_SECRET_KEY);
  }

  if (ok) {
    Serial.println("[INFO] MQTT connected");
  } else {
    Serial.printf("[WARN] MQTT connect failed, state=%d\n", g_mqtt.state());
  }
}

void addRiskFields(JsonDocument& doc, AlertLevel level, const char* reasonCode) {
  doc["risk_level"] = AlertProtocol::alertLevelToString(level);
  doc["risk_level_cn"] = AlertProtocol::alertLevelToChinese(level);
  doc["reason_code"] = reasonCode;
  doc["risk"] = AlertProtocol::alertLevelToString(level);
  doc["risk_cn"] = AlertProtocol::alertLevelToChinese(level);
}

void publishMqttJson(JsonDocument& doc, const char* label) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  ensureMqttConnected(millis());
  if (!g_mqtt.connected()) {
    return;
  }

  String payload;
  serializeJson(doc, payload);
  const bool ok = g_mqtt.publish(g_config.mqttTopic.c_str(), payload.c_str(), true);
  if (ok) {
    Serial.printf("[INFO] MQTT %s ok topic=%s\n", label, g_config.mqttTopic.c_str());
  } else {
    Serial.printf("[WARN] MQTT %s fail state=%d\n", label, g_mqtt.state());
  }
}

void beep(uint16_t durationMs, uint8_t repeat = 1) {
  for (uint8_t i = 0; i < repeat; ++i) {
    digitalWrite(ConfigPins::kBuzzer, HIGH);
    delay(durationMs);
    digitalWrite(ConfigPins::kBuzzer, LOW);
    delay(120);
  }
}

void maybeBeepForReason(const char* reasonCode) {
  if (AlertProtocol::reasonEquals(reasonCode, AlertProtocol::kReasonToiletStayWarning)) {
    beep(200, 2);
    return;
  }
  if (AlertProtocol::reasonEquals(reasonCode, AlertProtocol::kReasonToiletStayCritical)) {
    beep(250, 4);
    return;
  }
  if (AlertProtocol::reasonEquals(reasonCode, AlertProtocol::kReasonDaytimeBedrest)) {
    beep(200, 3);
    return;
  }
  if (AlertProtocol::reasonEquals(reasonCode, AlertProtocol::kReasonInactivity)) {
    beep(180, 2);
  }
}

void publishAlertForReason(const char* reasonCode, AlertLevel level, uint32_t nowMs) {
  JsonDocument doc;
  doc["type"] = AlertProtocol::kMessageTypeAlert;
  doc["title"] = AlertProtocol::reasonTitle(reasonCode);
  doc["detail"] = AlertProtocol::reasonDetail(reasonCode);
  doc["uptime_ms"] = nowMs;
  doc["time"] = formatNowTime();
  doc["bed_occupied"] = g_state.bedOccupied;
  doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["toilet_pir"] = g_state.toiletPir;
  doc["today_total_triggers"] = g_state.totalTriggersToday;
  doc["status_cn"] = AlertProtocol::alertLevelToChinese(level);
  addRiskFields(doc, level, reasonCode);
  publishMqttJson(doc, "alert");
  g_state.lastAlertReasonCode = reasonCode;
}

void resetDailyFlagsAndCounters() {
  g_state.bedroomPirTriggersToday = 0;
  g_state.toiletDoorChangesToday = 0;
  g_state.totalTriggersToday = 0;
  g_state.wakeupMinuteToday = ConfigRules::kInvalidWakeupMinute;
  g_state.wakeupWarnSent = false;
  g_state.toiletWarnSent = false;
  g_state.toiletCriticalSent = false;
  g_state.bedWarnSent = false;
  g_state.noActivityWarnSent = false;
  g_state.activityDropWarnSent = false;
  g_state.riskToday = AlertLevel::INFO;
  g_state.primaryReasonCode = AlertProtocol::kReasonNone;
  g_state.lastAlertReasonCode = AlertProtocol::kReasonNone;
  g_state.activeEvaluation = RuleEvaluation{};
}

void pushDayRecord(const DayRecord& record) {
  g_state.history[g_state.historyWriteIndex] = record;
  g_state.historyWriteIndex =
      (g_state.historyWriteIndex + 1) % ConfigRules::kHistoryDays;
  if (g_state.historyCount < ConfigRules::kHistoryDays) {
    g_state.historyCount++;
  }
}

void rolloverToDay(const TimeContext& ctx) {
  DayRecord done{};
  done.totalTriggers = g_state.totalTriggersToday;
  done.wakeupMinute = g_state.wakeupMinuteToday;
  pushDayRecord(done);
  g_state.dayKey = ctx.dayKey;
  g_state.dayKeyHasRealTime = ctx.hasRealTime;
  resetDailyFlagsAndCounters();
}

void rolloverDayIfNeeded(uint32_t nowMs) {
  const TimeContext ctx = buildTimeContext(nowMs);
  if (ctx.hasRealTime != g_state.dayKeyHasRealTime || ctx.dayKey != g_state.dayKey) {
    rolloverToDay(ctx);
  }
}

uint16_t averageTriggerCount() {
  if (g_state.historyCount == 0) {
    return 120;
  }
  uint32_t sum = 0;
  for (uint8_t i = 0; i < g_state.historyCount; ++i) {
    sum += g_state.history[i].totalTriggers;
  }
  return static_cast<uint16_t>(sum / g_state.historyCount);
}

uint16_t averageWakeupMinute() {
  uint32_t sum = 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < g_state.historyCount; ++i) {
    if (g_state.history[i].wakeupMinute != ConfigRules::kInvalidWakeupMinute) {
      sum += g_state.history[i].wakeupMinute;
      count++;
    }
  }
  if (count == 0) {
    return ConfigRules::kDefaultWakeupMinute;
  }
  return static_cast<uint16_t>(sum / count);
}

void scanSensors(uint32_t nowMs) {
  const bool bedroomPir = digitalRead(ConfigPins::kPirBedroom) == HIGH;
  const bool toiletPir = digitalRead(ConfigPins::kPirToilet) == HIGH;
  const bool toiletDoorClosed = digitalRead(ConfigPins::kDoorToilet) == HIGH;
  const int bedRaw = digitalRead(ConfigPins::kBedPressure);
  const bool bedOccupied =
      g_config.bedActiveLow ? (bedRaw == LOW) : (bedRaw == HIGH);

  const bool changed =
      (bedroomPir != g_state.lastBedroomPir) ||
      (toiletPir != g_state.lastToiletPir) ||
      (toiletDoorClosed != g_state.lastToiletDoorClosed) ||
      (bedOccupied != g_state.lastBedOccupied);
  if (changed) {
    g_state.sensorStateChanged = true;
  }

  if (bedroomPir && !g_state.lastBedroomPir) {
    g_state.bedroomPirTriggersToday++;
    g_state.totalTriggersToday++;
    g_state.lastActivityMs = nowMs;
  }
  if (toiletPir && !g_state.lastToiletPir) {
    g_state.totalTriggersToday++;
    g_state.lastActivityMs = nowMs;
  }
  if (toiletDoorClosed != g_state.lastToiletDoorClosed) {
    g_state.toiletDoorChangesToday++;
    g_state.totalTriggersToday++;
    g_state.lastActivityMs = nowMs;
  }

  const uint16_t minuteOfDay = buildTimeContext(nowMs).minuteOfDay;
  if (g_state.lastBedOccupied && !bedOccupied &&
      g_state.wakeupMinuteToday == ConfigRules::kInvalidWakeupMinute &&
      minuteOfDay >= 240 && minuteOfDay <= 720) {
    g_state.wakeupMinuteToday = minuteOfDay;
  }

  if (bedOccupied && !g_state.lastBedOccupied) {
    g_state.bedOccupiedStartMs = nowMs;
  }
  if (!bedOccupied) {
    g_state.bedOccupiedStartMs = 0;
    g_state.bedWarnSent = false;
    if (g_config.demoMode) {
      g_state.wakeupWarnSent = false;
    }
  }

  if (toiletDoorClosed && toiletPir) {
    if (g_state.toiletEnterMs == 0) {
      g_state.toiletEnterMs = nowMs;
    }
  } else {
    g_state.toiletEnterMs = 0;
    g_state.toiletWarnSent = false;
    g_state.toiletCriticalSent = false;
  }

  g_state.bedroomPir = bedroomPir;
  g_state.toiletPir = toiletPir;
  g_state.toiletDoorClosed = toiletDoorClosed;
  g_state.bedOccupied = bedOccupied;
  g_state.lastBedroomPir = bedroomPir;
  g_state.lastToiletPir = toiletPir;
  g_state.lastToiletDoorClosed = toiletDoorClosed;
  g_state.lastBedOccupied = bedOccupied;
}

RuleThresholds buildRuleThresholds() {
  RuleThresholds thresholds;
  thresholds.toiletWarnMs = g_config.demoMode ? ConfigRules::kDemoToiletWarnMs
                                              : ConfigRules::kToiletWarnMs;
  thresholds.toiletCriticalMs = g_config.demoMode ? ConfigRules::kDemoToiletWarnMs
                                                  : ConfigRules::kToiletCriticalMs;
  thresholds.bedWarnMs = g_config.demoMode ? ConfigRules::kDemoWarnMs
                                           : ConfigRules::kBedWarnMs;
  thresholds.noActivityWarnMs = g_config.demoMode ? ConfigRules::kDemoWarnMs
                                                  : ConfigRules::kNoActivityWarnMs;
  thresholds.demoWakeupStayMs = ConfigRules::kDemoWakeupStayMs;
  thresholds.wakeupToleranceMinutes = ConfigRules::kWakeupToleranceMinutes;
  thresholds.activityDropCheckMinute = ConfigRules::kActivityDropCheckMinute;
  thresholds.wakeBedroomPirMinTriggers = ConfigRules::kWakeBedroomPirMinTriggers;
  return thresholds;
}

RuleContext buildRuleContext(uint32_t nowMs) {
  const TimeContext timeCtx = buildTimeContext(nowMs);
  RuleContext context;
  context.nowMs = nowMs;
  context.minuteOfDay = timeCtx.minuteOfDay;
  context.demoMode = g_config.demoMode;
  context.bedOccupied = g_state.bedOccupied;
  context.bedOccupiedStartMs = g_state.bedOccupiedStartMs;
  context.toiletEnterMs = g_state.toiletEnterMs;
  context.lastActivityMs = g_state.lastActivityMs;
  context.bedroomPirTriggersToday = g_state.bedroomPirTriggersToday;
  context.totalTriggersToday = g_state.totalTriggersToday;
  context.wakeupBaselineMinute = averageWakeupMinute();
  context.activityBaseline = averageTriggerCount();
  return context;
}

void refreshEvaluation(uint32_t nowMs) {
  const RuleEvaluation previous = g_state.activeEvaluation;
  g_state.activeEvaluation = EvaluateRuleSet(buildRuleContext(nowMs), buildRuleThresholds());
  g_state.riskToday = g_state.activeEvaluation.primaryLevel;
  g_state.primaryReasonCode = g_state.activeEvaluation.primaryReasonCode;

  const bool changed =
      previous.primaryLevel != g_state.activeEvaluation.primaryLevel ||
      !AlertProtocol::reasonEquals(
          previous.primaryReasonCode,
          g_state.activeEvaluation.primaryReasonCode);
  if (changed) {
    g_state.sensorStateChanged = true;
  }
}

void processRuleNotifications(uint32_t nowMs) {
  const RuleEvaluation& eval = g_state.activeEvaluation;

  if (eval.wakeRuleActive && !g_state.wakeupWarnSent) {
    g_state.wakeupWarnSent = true;
    publishAlertForReason(AlertProtocol::kReasonLateWakeup, AlertLevel::WARNING, nowMs);
  }
  if (!eval.wakeRuleActive && g_config.demoMode) {
    g_state.wakeupWarnSent = false;
  }

  if (eval.toiletWarningActive && !g_state.toiletWarnSent) {
    g_state.toiletWarnSent = true;
    maybeBeepForReason(AlertProtocol::kReasonToiletStayWarning);
    publishAlertForReason(
        AlertProtocol::kReasonToiletStayWarning,
        AlertLevel::WARNING,
        nowMs);
  }
  if (eval.toiletCriticalActive && !g_state.toiletCriticalSent) {
    g_state.toiletCriticalSent = true;
    maybeBeepForReason(AlertProtocol::kReasonToiletStayCritical);
    publishAlertForReason(
        AlertProtocol::kReasonToiletStayCritical,
        AlertLevel::CRITICAL,
        nowMs);
  }
  if (!eval.toiletWarningActive && !eval.toiletCriticalActive) {
    g_state.toiletWarnSent = false;
    g_state.toiletCriticalSent = false;
  }

  if (eval.bedrestActive && !g_state.bedWarnSent) {
    g_state.bedWarnSent = true;
    maybeBeepForReason(AlertProtocol::kReasonDaytimeBedrest);
    publishAlertForReason(
        AlertProtocol::kReasonDaytimeBedrest,
        AlertLevel::WARNING,
        nowMs);
  }
  if (!eval.bedrestActive) {
    g_state.bedWarnSent = false;
  }

  if (eval.inactivityActive && !g_state.noActivityWarnSent) {
    g_state.noActivityWarnSent = true;
    maybeBeepForReason(AlertProtocol::kReasonInactivity);
    publishAlertForReason(AlertProtocol::kReasonInactivity, AlertLevel::WARNING, nowMs);
  }
  if (!eval.inactivityActive) {
    g_state.noActivityWarnSent = false;
  }

  if (eval.activityDropActive && !g_state.activityDropWarnSent) {
    g_state.activityDropWarnSent = true;
    publishAlertForReason(
        AlertProtocol::kReasonActivityDrop,
        AlertLevel::WARNING,
        nowMs);
  }
}

void populateStatusDoc(JsonDocument& doc, uint32_t nowMs) {
  const TimeContext timeCtx = buildTimeContext(nowMs);
  doc["type"] = AlertProtocol::kMessageTypeStatus;
  doc["status_cn"] = AlertProtocol::alertLevelToChinese(g_state.riskToday);
  doc["minute_of_day"] = timeCtx.minuteOfDay;
  doc["using_real_time"] = timeCtx.hasRealTime;
  doc["wakeup_recorded"] =
      g_state.wakeupMinuteToday != ConfigRules::kInvalidWakeupMinute;
  doc["bedroom_pir"] = g_state.bedroomPir;
  doc["bed_occupied"] = g_state.bedOccupied;
  doc["bed_raw"] = digitalRead(ConfigPins::kBedPressure);
  doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["toilet_pir"] = g_state.toiletPir;
  doc["door_raw"] = digitalRead(ConfigPins::kDoorToilet);
  doc["buzzer_raw"] = digitalRead(ConfigPins::kBuzzer);
  doc["today_total_triggers"] = g_state.totalTriggersToday;
  doc["avg_total_triggers_5d"] = averageTriggerCount();
  doc["avg_wakeup_minute_5d"] = averageWakeupMinute();
  doc["wakeup_warn"] = g_state.activeEvaluation.wakeRuleActive;
  doc["toilet_warn"] = g_state.activeEvaluation.toiletWarningActive;
  doc["toilet_critical"] = g_state.activeEvaluation.toiletCriticalActive;
  doc["bed_warn"] = g_state.activeEvaluation.bedrestActive;
  doc["no_activity_warn"] = g_state.activeEvaluation.inactivityActive;
  doc["activity_drop_warn"] = g_state.activeEvaluation.activityDropActive;
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["wifi_ssid"] = g_config.ssid;
  doc["wifi_ip"] =
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0";
  doc["ap_mode"] = g_apConfigMode;
  doc["ap_ssid"] = ConfigDefaults::kApSsid;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["demo_mode"] = g_config.demoMode;
  doc["bed_active_low"] = g_config.bedActiveLow;
  doc["pin_pir_bedroom"] = ConfigPins::kPirBedroom;
  doc["pin_pir_toilet"] = ConfigPins::kPirToilet;
  doc["pin_door_toilet"] = ConfigPins::kDoorToilet;
  doc["pin_bed_pressure"] = ConfigPins::kBedPressure;
  doc["pin_buzzer"] = ConfigPins::kBuzzer;
  doc["time_synced"] = g_timeSynced;
  doc["now_time"] = formatNowTime();
  doc["last_alert_reason_code"] = g_state.lastAlertReasonCode;
  addRiskFields(doc, g_state.riskToday, g_state.primaryReasonCode);
}

void publishHeartbeatIfNeeded(uint32_t nowMs) {
  if (nowMs - g_state.lastMqttHeartbeatMs < ConfigDefaults::kMqttHeartbeatMs) {
    return;
  }
  g_state.lastMqttHeartbeatMs = nowMs;
  JsonDocument doc;
  populateStatusDoc(doc, nowMs);
  publishMqttJson(doc, "heartbeat");
}

void publishSensorRealtimeIfNeeded(uint32_t nowMs) {
  const bool periodicDue =
      (nowMs - g_state.lastMqttSensorPublishMs) >= ConfigDefaults::kMqttSensorPublishMs;
  if (!g_state.sensorStateChanged && !periodicDue) {
    return;
  }

  JsonDocument doc;
  doc["type"] = AlertProtocol::kMessageTypeSensorRealtime;
  doc["time"] = formatNowTime();
  doc["uptime_ms"] = nowMs;
  doc["bedroom_pir"] = g_state.bedroomPir;
  doc["toilet_pir"] = g_state.toiletPir;
  doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["bed_occupied"] = g_state.bedOccupied;
  doc["pin_pir_bedroom"] = ConfigPins::kPirBedroom;
  doc["pin_pir_toilet"] = ConfigPins::kPirToilet;
  doc["pin_door_toilet"] = ConfigPins::kDoorToilet;
  doc["pin_bed_pressure"] = ConfigPins::kBedPressure;
  doc["pin_buzzer"] = ConfigPins::kBuzzer;
  doc["analog_pir_bedroom"] = -1;
  doc["analog_pir_toilet"] = -1;
  doc["analog_door_toilet"] = -1;
  doc["analog_bed_pressure"] = -1;
  doc["today_total_triggers"] = g_state.totalTriggersToday;
  addRiskFields(doc, g_state.riskToday, g_state.primaryReasonCode);
  publishMqttJson(doc, "realtime");

  g_state.lastMqttSensorPublishMs = nowMs;
  g_state.sensorStateChanged = false;
}

void applyRiskFallbackNow(uint32_t nowMs) {
  refreshEvaluation(nowMs);
}

void printPinSnapshot() {
  Serial.printf(
      "PIR_BED(GPIO%d) d=%d | PIR_TOI(GPIO%d) d=%d | DOOR(GPIO%d) d=%d | BED(GPIO%d) d=%d | BUZ(GPIO%d)\n",
      ConfigPins::kPirBedroom,
      digitalRead(ConfigPins::kPirBedroom),
      ConfigPins::kPirToilet,
      digitalRead(ConfigPins::kPirToilet),
      ConfigPins::kDoorToilet,
      digitalRead(ConfigPins::kDoorToilet),
      ConfigPins::kBedPressure,
      digitalRead(ConfigPins::kBedPressure),
      ConfigPins::kBuzzer);
}

void printWifiStatus() {
  Serial.printf(
      "wifi=%d ip=%s ap=%s ap_ip=%s ssid=%s mqtt=%s:%u topic=%s time=%s\n",
      static_cast<int>(WiFi.status()),
      WiFi.localIP().toString().c_str(),
      g_apConfigMode ? "on" : "off",
      WiFi.softAPIP().toString().c_str(),
      g_config.ssid.c_str(),
      g_config.mqttBroker.c_str(),
      g_config.mqttPort,
      g_config.mqttTopic.c_str(),
      formatNowTime().c_str());
}

void printSerialHelp() {
  Serial.println(
      "help | pins | pinmap | d <pin> | a <pin> | wifi | setwifi <ssid> <pwd> | reconnect | demo on|off | time");
}

void processSerialCommand(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (line.length() == 0) {
    return;
  }
  if (line == "help") {
    printSerialHelp();
    return;
  }
  if (line == "pins" || line == "pinmap") {
    printPinSnapshot();
    return;
  }
  if (line == "wifi") {
    printWifiStatus();
    return;
  }
  if (line == "time") {
    Serial.printf("time_synced=%s now=%s\n", g_timeSynced ? "true" : "false", formatNowTime().c_str());
    return;
  }
  if (line == "reconnect") {
    stopApConfigMode();
    g_wifiConnectFailCount = 0;
    WiFi.disconnect(true, false);
    g_lastWifiBeginMs = 0;
    ensureWifiConnected();
    return;
  }
  if (line == "demo on" || line == "demo off") {
    const bool oldDemoMode = g_config.demoMode;
    g_config.demoMode = (line == "demo on");
    if (oldDemoMode && !g_config.demoMode) {
      g_state.wakeupWarnSent = false;
      g_state.toiletWarnSent = false;
      g_state.toiletCriticalSent = false;
      g_state.bedWarnSent = false;
      g_state.noActivityWarnSent = false;
      g_state.activityDropWarnSent = false;
    }
    saveConfig();
    Serial.printf("demo_mode=%s\n", g_config.demoMode ? "ON" : "OFF");
    return;
  }
  if (line.startsWith("d ")) {
    int pin = line.substring(2).toInt();
    pinMode(pin, INPUT);
    Serial.printf("digitalRead(GPIO%d)=%d\n", pin, digitalRead(pin));
    if (isManagedSensorOrActuatorPin(pin)) {
      applyPinModes();
    }
    return;
  }
  if (line.startsWith("a ")) {
    int pin = line.substring(2).toInt();
    pinMode(pin, INPUT);
    Serial.printf("analogRead(GPIO%d)=%d\n", pin, safeAnalogRead(pin));
    if (isManagedSensorOrActuatorPin(pin)) {
      applyPinModes();
    }
    return;
  }
  if (line.startsWith("setwifi ")) {
    int firstSpace = line.indexOf(' ');
    int secondSpace = line.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0) {
      Serial.println("usage: setwifi <ssid> <pwd>");
      return;
    }
    g_config.ssid = line.substring(firstSpace + 1, secondSpace);
    g_config.pass = line.substring(secondSpace + 1);
    g_config.ssid.trim();
    g_config.pass.trim();
    if (g_config.ssid.length() == 0) {
      Serial.println("SSID cannot be empty.");
      return;
    }
    saveConfig();
    stopApConfigMode();
    g_wifiConnectFailCount = 0;
    WiFi.disconnect(true, false);
    g_lastWifiBeginMs = 0;
    ensureWifiConnected();
    return;
  }
  Serial.printf("unknown command: %s\n", line.c_str());
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    g_lastSerialInputMs = millis();
    if (c == '\r' || c == '\n' || c == ';') {
      if (g_serialLine.length() == 0) {
        continue;
      }
      processSerialCommand(g_serialLine);
      g_serialLine = "";
    } else {
      g_serialLine += c;
      if (g_serialLine.length() > 180) {
        g_serialLine = "";
        Serial.println("command too long, cleared.");
      }
    }
  }
  if (g_serialLine.length() > 0 && (millis() - g_lastSerialInputMs > 500)) {
    processSerialCommand(g_serialLine);
    g_serialLine = "";
  }
}

void handleApiStatus() {
  JsonDocument doc;
  populateStatusDoc(doc, millis());
  String payload;
  serializeJson(doc, payload);
  g_web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  g_web.sendHeader("Pragma", "no-cache");
  g_web.sendHeader("Expires", "0");
  g_web.send(200, "application/json; charset=utf-8", payload);
}

void handleApiWifiConfig() {
  if (!g_web.hasArg("ssid")) {
    g_web.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"missing ssid\"}");
    return;
  }
  g_config.ssid = g_web.arg("ssid");
  g_config.pass = g_web.arg("pass");
  g_config.ssid.trim();
  g_config.pass.trim();
  if (g_config.ssid.length() == 0) {
    g_web.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"ssid empty\"}");
    return;
  }
  saveConfig();
  stopApConfigMode();
  g_wifiConnectFailCount = 0;
  WiFi.disconnect(true, false);
  g_lastWifiBeginMs = 0;
  ensureWifiConnected();
  g_web.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"msg\":\"saved and reconnecting\"}");
}

void handleApiDevConfig() {
  if (g_web.method() == HTTP_GET) {
    JsonDocument doc;
    doc["demo_mode"] = g_config.demoMode;
    doc["bed_active_low"] = g_config.bedActiveLow;
    doc["mqtt_broker"] = g_config.mqttBroker;
    doc["mqtt_port"] = g_config.mqttPort;
    doc["mqtt_topic"] = g_config.mqttTopic;
    doc["pin_pir_bedroom"] = ConfigPins::kPirBedroom;
    doc["pin_pir_toilet"] = ConfigPins::kPirToilet;
    doc["pin_door_toilet"] = ConfigPins::kDoorToilet;
    doc["pin_bed_pressure"] = ConfigPins::kBedPressure;
    doc["pin_buzzer"] = ConfigPins::kBuzzer;
    String payload;
    serializeJson(doc, payload);
    g_web.send(200, "application/json; charset=utf-8", payload);
    return;
  }

  const bool oldDemoMode = g_config.demoMode;
  if (g_web.hasArg("demo_mode")) {
    String mode = g_web.arg("demo_mode");
    g_config.demoMode = (mode == "1" || mode == "true" || mode == "on");
  }
  if (g_web.hasArg("bed_active_low")) {
    String mode = g_web.arg("bed_active_low");
    g_config.bedActiveLow = (mode == "1" || mode == "true" || mode == "on");
  }
  if (g_web.hasArg("mqtt_broker")) {
    g_config.mqttBroker = g_web.arg("mqtt_broker");
    g_config.mqttBroker.trim();
  }
  if (g_web.hasArg("mqtt_port")) {
    const uint16_t port = static_cast<uint16_t>(g_web.arg("mqtt_port").toInt());
    if (port != 0) {
      g_config.mqttPort = port;
    }
  }
  if (g_web.hasArg("mqtt_topic")) {
    g_config.mqttTopic = g_web.arg("mqtt_topic");
    g_config.mqttTopic.trim();
  }

  if (oldDemoMode && !g_config.demoMode) {
    g_state.wakeupWarnSent = false;
    g_state.toiletWarnSent = false;
    g_state.toiletCriticalSent = false;
    g_state.bedWarnSent = false;
    g_state.noActivityWarnSent = false;
    g_state.activityDropWarnSent = false;
  }
  saveConfig();
  g_web.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleApiDevRiskDrop() {
  applyRiskFallbackNow(millis());
  g_web.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleCaptiveRedirect() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  g_web.sendHeader("Location", url, true);
  g_web.send(302, "text/plain", "Captive portal");
}

String htmlPage() {
  return String(
      "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>独居老人隐私预警系统</title><style>body{font-family:Arial,sans-serif;background:#f6f7fb;padding:20px;color:#1d2433}"
      ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 3px 12px rgba(0,0,0,.08)}"
      ".risk-normal{color:#147a3d;font-weight:700}.risk-warning{color:#a66b00;font-weight:700}.risk-critical{color:#b5121b;font-weight:700}"
      ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}@media(max-width:760px){.grid{grid-template-columns:1fr}}</style></head><body>"
      "<div class='card'><div id='riskText' class='risk-normal'>加载中...</div><div>当前时间：<span id='nowTime'>-</span></div><div>主要原因：<span id='reasonCode'>-</span></div><div>最近告警：<span id='lastReason'>-</span></div></div>"
      "<div class='card grid'><div>今日活动触发：<span id='total'>-</span></div><div>5日平均触发：<span id='avg'>-</span></div><div>卧床状态：<span id='bed'>-</span></div><div>厕所状态：<span id='toilet'>-</span></div><div>起床异常：<span id='wakeup'>-</span></div><div>活动量骤降：<span id='drop'>-</span></div></div>"
      "<div class='card'><div>联网状态：<span id='net'>-</span></div><div>设备 IP：<span id='ip'>-</span></div><div id='apTip' style='display:none'>AP 配网：<span id='apSsid'></span> @ <span id='apIp'></span></div><div id='cfgForm' style='display:none;margin-top:10px'><input id='ssid' placeholder='Wi-Fi SSID'><input id='pass' type='password' placeholder='Wi-Fi 密码'><button onclick='saveWifi()'>提交配网</button></div></div>"
      "<script>async function pull(){try{const r=await fetch('/api/status?t='+Date.now(),{cache:'no-store'});const d=await r.json();const risk=document.getElementById('riskText');risk.textContent='风险等级：'+d.risk_cn+'（状态：'+d.status_cn+'）';risk.className=d.risk_level==='critical'?'risk-critical':(d.risk_level==='warning'?'risk-warning':'risk-normal');document.getElementById('nowTime').textContent=d.now_time;document.getElementById('reasonCode').textContent=d.reason_code||'-';document.getElementById('lastReason').textContent=d.last_alert_reason_code||'-';document.getElementById('total').textContent=d.today_total_triggers;document.getElementById('avg').textContent=d.avg_total_triggers_5d;document.getElementById('bed').textContent=d.bed_occupied?'床上有人':'已离床';document.getElementById('toilet').textContent=(d.toilet_door_closed&&d.toilet_pir)?'疑似滞留':'正常';document.getElementById('wakeup').textContent=d.wakeup_warn?'异常':'正常';document.getElementById('drop').textContent=d.activity_drop_warn?'异常':'正常';document.getElementById('net').textContent=d.wifi_connected?('已联网：'+d.wifi_ssid):'未联网';document.getElementById('ip').textContent=d.wifi_ip;document.getElementById('cfgForm').style.display=d.wifi_connected?'none':'block';document.getElementById('apTip').style.display=d.wifi_connected?'none':'block';document.getElementById('apSsid').textContent=d.ap_ssid;document.getElementById('apIp').textContent=d.ap_ip;}catch(e){document.getElementById('net').textContent='状态拉取失败';}}"
      "async function saveWifi(){const ssid=document.getElementById('ssid').value;const pass=document.getElementById('pass').value;if(!ssid){alert('SSID 不能为空');return;}const b='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);const r=await fetch('/api/wifi-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const d=await r.json();alert(d.ok?'已提交配网':'提交失败');}"
      "pull();setInterval(pull,1000);</script></body></html>");
}

void setupWebServer() {
  g_web.on("/", HTTP_GET, []() { g_web.send(200, "text/html; charset=utf-8", htmlPage()); });
  g_web.on("/api/status", HTTP_GET, handleApiStatus);
  g_web.on("/api/wifi-config", HTTP_POST, handleApiWifiConfig);
  g_web.on("/api/dev-config", HTTP_GET, handleApiDevConfig);
  g_web.on("/api/dev-config", HTTP_POST, handleApiDevConfig);
  g_web.on("/api/dev-risk-drop", HTTP_POST, handleApiDevRiskDrop);
  g_web.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/gen_204", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/library/test/success.html", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/fwlink", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);
  g_web.on("/ncsi.txt", HTTP_GET, handleCaptiveRedirect);
  g_web.onNotFound([]() {
    if (g_apConfigMode) {
      handleCaptiveRedirect();
      return;
    }
    g_web.send(404, "text/plain", "Not found");
  });
  g_web.begin();
}

void setup() {
  Serial.begin(115200);
  Serial.println("[INFO] Serial command ready. baud=115200.");
  SPIFFS.begin(true);
  loadConfig();
  applyPinModes();

  const uint32_t nowMs = millis();
  g_state.lastActivityMs = nowMs;

  ensureWifiConnected();
  setupWebServer();
  ensureMqttConnected(nowMs);
  updateTimeSyncStatus(nowMs);

  const TimeContext timeCtx = buildTimeContext(nowMs);
  g_state.dayKey = timeCtx.dayKey;
  g_state.dayKeyHasRealTime = timeCtx.hasRealTime;
  refreshEvaluation(nowMs);
}

void loop() {
  const uint32_t nowMs = millis();
  rolloverDayIfNeeded(nowMs);
  ensureWifiConnected();
  updateTimeSyncStatus(nowMs);
  scanSensors(nowMs);

  ensureMqttConnected(nowMs);
  g_mqtt.loop();
  if (g_dnsRunning) {
    g_dns.processNextRequest();
  }

  refreshEvaluation(nowMs);
  processRuleNotifications(nowMs);

  g_web.handleClient();
  handleSerialCommands();
  publishSensorRealtimeIfNeeded(nowMs);
  publishHeartbeatIfNeeded(nowMs);
  delay(50);
}
