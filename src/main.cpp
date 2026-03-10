
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>

/*
  引脚/接线示意图（程序内固定常量，可在此处改动后重新编译）
  ------------------------------------------------------------
  卧室 PIR 传感器        -> GPIO34   (数字输入)
  厕所 PIR 传感器        -> GPIO35   (数字输入)
  厕所门磁传感器         -> GPIO32   (数字输入，按当前逻辑 HIGH=门关)
  床压传感器             -> GPIO33   (数字输入，默认不启用内部上拉，极性由 bedActiveLow 决定)
  蜂鸣器                 -> GPIO27   (数字输出)
*/
constexpr uint8_t PIN_DEFAULT_PIR_BEDROOM = 34;
constexpr uint8_t PIN_DEFAULT_PIR_TOILET = 35;
constexpr uint8_t PIN_DEFAULT_DOOR_TOILET = 32;
constexpr uint8_t PIN_DEFAULT_BED_PRESSURE = 33;
constexpr uint8_t PIN_DEFAULT_BUZZER = 27;
constexpr bool BED_PRESSURE_USE_PULLUP = false;

const char* AP_SSID = "SeniorAlert-Config";
const char* AP_PASS = "12345678";
const char* MQTT_BROKER_ADDR = "bemfa.com";
constexpr uint16_t MQTT_BROKER_PORT = 9501;
const char* MQTT_CLIENT_ID = "";
const char* MQTT_USERNAME = "";
const char* MQTT_PRIVATE_KEY = "84810b9b5f5245fdbc1e1738837f27a9";
const char* MQTT_ALERT_TOPIC = "senior/alert/events";

constexpr uint32_t MINUTE_MS = 60000UL;
constexpr uint32_t HOUR_MS = 3600000UL;
constexpr uint32_t DAY_MS = 86400000UL;
constexpr uint32_t TOILET_WARN_MS = 12UL * MINUTE_MS;
constexpr uint32_t TOILET_CRITICAL_MS = 18UL * MINUTE_MS;
constexpr uint32_t BED_WARN_MS = 3UL * HOUR_MS;
constexpr uint32_t NO_ACTIVITY_WARN_MS = 2UL * HOUR_MS;
constexpr uint32_t DEMO_WARN_MS = 10000UL;
constexpr uint32_t DEMO_WAKEUP_STAY_MS = 10000UL;
constexpr uint32_t DEMO_TOILET_WARN_MS = 1000UL;
constexpr uint32_t MQTT_HEARTBEAT_MS = 30000UL;
constexpr uint32_t MQTT_SENSOR_PUBLISH_MS = 1000UL;
constexpr uint8_t WIFI_FAIL_TO_AP_THRESHOLD = 3;
constexpr uint16_t DEFAULT_WAKEUP_MINUTE = 7 * 60;
constexpr uint16_t WAKEUP_TOLERANCE_MINUTES = 90;
constexpr uint8_t HISTORY_DAYS = 5;
constexpr uint16_t INVALID_WAKEUP_MINUTE = 0xFFFF;
const char* CONFIG_PATH = "/config.json";

enum class AlertLevel : uint8_t { INFO = 0, WARNING = 1, CRITICAL = 2 };

struct DayRecord { uint16_t totalTriggers = 0; uint16_t wakeupMinute = INVALID_WAKEUP_MINUTE; };
struct AppConfig {
  String ssid = "";
  String pass = "";
  bool demoMode = false;
  bool bedActiveLow = true;
};
struct RuntimeState {
  bool bedroomPir = false, toiletPir = false, toiletDoorClosed = false, bedOccupied = false;
  bool lastBedroomPir = false, lastToiletPir = false, lastToiletDoorClosed = false, lastBedOccupied = false;
  uint32_t lastActivityMs = 0, bedOccupiedStartMs = 0, toiletEnterMs = 0;
  uint32_t lastMqttRetryMs = 0, lastMqttHeartbeatMs = 0, lastMqttSensorPublishMs = 0;
  bool sensorStateChanged = false;
  uint32_t uptimeDay = 0;
  uint16_t bedroomPirTriggersToday = 0, toiletDoorChangesToday = 0, totalTriggersToday = 0, wakeupMinuteToday = INVALID_WAKEUP_MINUTE;
  bool wakeupWarnSent = false, toiletWarnSent = false, toiletCriticalSent = false, bedWarnSent = false, noActivityWarnSent = false, activityDropWarnSent = false;
  AlertLevel riskToday = AlertLevel::INFO;
  DayRecord history[HISTORY_DAYS];
  uint8_t historyWriteIndex = 0, historyCount = 0;
};

WiFiClient g_wifiClient;
PubSubClient g_mqtt(g_wifiClient);
WebServer g_web(80);
DNSServer g_dns;
RuntimeState g_state;
AppConfig g_config;

bool g_wifiStackReady = false, g_apConfigMode = false, g_timeSynced = false, g_timeSyncRequested = false;
bool g_dnsRunning = false;
uint8_t g_wifiConnectFailCount = 0;
uint32_t g_lastWifiBeginMs = 0, g_lastTimeSyncTryMs = 0, g_lastSerialInputMs = 0;
uint32_t g_lastAdc2WarnMs = 0;
String g_serialLine;

constexpr byte DNS_PORT = 53;

void applyPinModes() {
  pinMode(PIN_DEFAULT_PIR_BEDROOM, INPUT);
  pinMode(PIN_DEFAULT_PIR_TOILET, INPUT);
  pinMode(PIN_DEFAULT_DOOR_TOILET, INPUT_PULLUP);
  pinMode(PIN_DEFAULT_BED_PRESSURE, BED_PRESSURE_USE_PULLUP ? INPUT_PULLUP : INPUT);
  pinMode(PIN_DEFAULT_BUZZER, OUTPUT);
  digitalWrite(PIN_DEFAULT_BUZZER, LOW);
}

const char* alertLevelToString(AlertLevel level) {
  switch (level) { case AlertLevel::INFO: return "normal"; case AlertLevel::WARNING: return "warning"; case AlertLevel::CRITICAL: return "critical"; default: return "unknown"; }
}
const char* alertLevelToChinese(AlertLevel level) {
  switch (level) { case AlertLevel::INFO: return "正常"; case AlertLevel::WARNING: return "中度异常"; case AlertLevel::CRITICAL: return "高风险"; default: return "未知"; }
}
uint16_t minuteOfDayFromUptime(uint32_t nowMs) { return static_cast<uint16_t>((nowMs / MINUTE_MS) % (24UL * 60UL)); }
bool isSleepWindow(uint16_t m) { return (m >= 23 * 60) || (m <= 6 * 60); }
bool isDaytimeWindow(uint16_t m) { return m >= 8 * 60 && m <= 21 * 60; }
uint32_t toiletWarnThresholdMs() { return g_config.demoMode ? DEMO_TOILET_WARN_MS : TOILET_WARN_MS; }
uint32_t toiletCriticalThresholdMs() { return g_config.demoMode ? DEMO_TOILET_WARN_MS : TOILET_CRITICAL_MS; }
uint32_t bedWarnThresholdMs() { return g_config.demoMode ? DEMO_WARN_MS : BED_WARN_MS; }
uint32_t noActivityWarnThresholdMs() { return g_config.demoMode ? DEMO_WARN_MS : NO_ACTIVITY_WARN_MS; }

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
        Serial.printf("[WARN] analogRead(GPIO%d) skipped: ADC2 unavailable while Wi-Fi is active.\n", pin);
      }
    }
    return -1;
  }
  return analogRead(pin);
}

bool isManagedSensorOrActuatorPin(int pin) {
  return pin == PIN_DEFAULT_PIR_BEDROOM ||
         pin == PIN_DEFAULT_PIR_TOILET ||
         pin == PIN_DEFAULT_DOOR_TOILET ||
         pin == PIN_DEFAULT_BED_PRESSURE ||
         pin == PIN_DEFAULT_BUZZER;
}

String formatNowTime() {
  if (!g_timeSynced) return "N/A";
  tm now{}; if (!getLocalTime(&now, 10)) return "N/A";
  char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now); return String(buf);
}

bool saveConfig() {
  JsonDocument doc;
  doc["config_version"] = 3;
  doc["ssid"] = g_config.ssid;
  doc["pass"] = g_config.pass;
  doc["demo_mode"] = g_config.demoMode;
  doc["bed_active_low"] = g_config.bedActiveLow;
  File f = SPIFFS.open(CONFIG_PATH, FILE_WRITE); if (!f) return false; serializeJson(doc, f); f.close(); return true;
}

void loadConfig() {
  if (!SPIFFS.exists(CONFIG_PATH)) return;
  File f = SPIFFS.open(CONFIG_PATH, FILE_READ); if (!f) return;
  JsonDocument doc; if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  g_config.ssid = doc["ssid"] | "";
  g_config.pass = doc["pass"] | "";
  g_config.demoMode = doc["demo_mode"] | false;
  g_config.bedActiveLow = doc["bed_active_low"] | true;
}
void requestTimeSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(8 * 3600, 0, "ntp1.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  g_timeSyncRequested = true; g_lastTimeSyncTryMs = millis();
}

void updateTimeSyncStatus(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED) { g_timeSynced = false; return; }
  if (!g_timeSyncRequested || (nowMs - g_lastTimeSyncTryMs > 30000)) requestTimeSync();
  tm now{}; if (getLocalTime(&now, 10)) g_timeSynced = true;
}

void ensureWifiStackReady() {
  if (g_wifiStackReady) return;
  WiFi.persistent(false); WiFi.mode(WIFI_STA); g_wifiStackReady = true;
}

void startApConfigMode() {
  if (g_apConfigMode) return;
  WiFi.mode(WIFI_AP);
  g_apConfigMode = WiFi.softAP(AP_SSID, AP_PASS);
  if (g_apConfigMode) {
    g_dns.start(DNS_PORT, "*", WiFi.softAPIP());
    g_dnsRunning = true;
    Serial.printf("[INFO] AP provisioning started: %s %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

void stopApConfigMode() {
  if (!g_apConfigMode) return;
  if (g_dnsRunning) {
    g_dns.stop();
    g_dnsRunning = false;
  }
  WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); g_apConfigMode = false;
}

void ensureWifiConnected() {
  ensureWifiStackReady();
  if (WiFi.status() == WL_CONNECTED) { g_wifiConnectFailCount = 0; stopApConfigMode(); return; }
  if (g_config.ssid.length() == 0) { startApConfigMode(); return; }
  if (g_apConfigMode) return;
  if (millis() - g_lastWifiBeginMs < 5000) return;
  g_lastWifiBeginMs = millis();

  WiFi.mode(WIFI_STA); WiFi.begin(g_config.ssid.c_str(), g_config.pass.c_str());
  uint8_t retry = 0; while (WiFi.status() != WL_CONNECTED && retry < 10) { delay(300); retry++; }

  if (WiFi.status() == WL_CONNECTED) {
    g_wifiConnectFailCount = 0; stopApConfigMode(); requestTimeSync();
  } else {
    if (g_wifiConnectFailCount < 255) g_wifiConnectFailCount++;
    if (g_wifiConnectFailCount >= WIFI_FAIL_TO_AP_THRESHOLD) startApConfigMode();
  }
}

String mqttClientId() {
  if (strlen(MQTT_CLIENT_ID) > 0) return String(MQTT_CLIENT_ID);
  uint64_t chipId = ESP.getEfuseMac();
  return String("senior-alert-") + String(static_cast<uint32_t>(chipId & 0xFFFFFF), HEX);
}

void ensureMqttConnected(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED || g_mqtt.connected()) return;
  if (nowMs - g_state.lastMqttRetryMs < 5000) return;
  g_state.lastMqttRetryMs = nowMs;

  g_mqtt.setServer(MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  const String cid = mqttClientId();
  bool ok = g_mqtt.connect(cid.c_str(), MQTT_USERNAME, MQTT_PRIVATE_KEY);
  if (!ok) {
    Serial.printf("[WARN] MQTT mode-1 failed, state=%d; fallback to mode-2/3/4\n", g_mqtt.state());
  }

  // Fallback mode-2: BEMFA common form (username=private key, empty password)
  if (!ok) ok = g_mqtt.connect(cid.c_str(), MQTT_PRIVATE_KEY, "");
  // Fallback mode-3: reverse form (empty username, private key as password)
  if (!ok) ok = g_mqtt.connect(cid.c_str(), "", MQTT_PRIVATE_KEY);
  // Fallback mode-4: anonymous
  if (!ok) ok = g_mqtt.connect(cid.c_str());

  if (ok) {
    Serial.printf("[INFO] MQTT connected, clientId=%s\n", cid.c_str());
  } else {
    Serial.printf("[WARN] MQTT connect failed after all modes, final state=%d\n", g_mqtt.state());
  }
}

void publishMqttJson(JsonDocument& doc, const char* label) {
  if (WiFi.status() != WL_CONNECTED) return;
  ensureMqttConnected(millis());
  if (!g_mqtt.connected()) return;
  String payload; serializeJson(doc, payload);
  const bool ok = g_mqtt.publish(MQTT_ALERT_TOPIC, payload.c_str(), true);
  Serial.printf("[INFO] MQTT %s %s\n", label, ok ? "ok" : "fail");
}

void publishAlert(const char* title, const char* detail, AlertLevel level) {
  JsonDocument doc;
  doc["type"] = "alert"; doc["title"] = title; doc["detail"] = detail;
  doc["risk"] = alertLevelToString(level); doc["risk_cn"] = alertLevelToChinese(level);
  doc["uptime_ms"] = millis(); doc["time"] = formatNowTime();
  doc["bed_occupied"] = g_state.bedOccupied; doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["toilet_pir"] = g_state.toiletPir; doc["today_total_triggers"] = g_state.totalTriggersToday;
  publishMqttJson(doc, "alert");
}

void publishHeartbeatIfNeeded(uint32_t nowMs) {
  if (nowMs - g_state.lastMqttHeartbeatMs < MQTT_HEARTBEAT_MS) return;
  g_state.lastMqttHeartbeatMs = nowMs;
  JsonDocument doc;
  doc["type"] = "status"; doc["uptime_ms"] = nowMs; doc["time"] = formatNowTime();
  doc["risk"] = alertLevelToString(g_state.riskToday); doc["risk_cn"] = alertLevelToChinese(g_state.riskToday);
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED; doc["ap_mode"] = g_apConfigMode;
  doc["demo_mode"] = g_config.demoMode; doc["today_total_triggers"] = g_state.totalTriggersToday;
  publishMqttJson(doc, "heartbeat");
}

void publishSensorRealtimeIfNeeded(uint32_t nowMs) {
  const bool periodicDue = (nowMs - g_state.lastMqttSensorPublishMs) >= MQTT_SENSOR_PUBLISH_MS;
  if (!g_state.sensorStateChanged && !periodicDue) return;

  JsonDocument doc;
  doc["type"] = "sensor_realtime"; doc["uptime_ms"] = nowMs; doc["time"] = formatNowTime();
  doc["bedroom_pir"] = g_state.bedroomPir; doc["toilet_pir"] = g_state.toiletPir;
  doc["toilet_door_closed"] = g_state.toiletDoorClosed; doc["bed_occupied"] = g_state.bedOccupied;
  doc["pin_pir_bedroom"] = PIN_DEFAULT_PIR_BEDROOM;
  doc["pin_pir_toilet"] = PIN_DEFAULT_PIR_TOILET;
  doc["pin_door_toilet"] = PIN_DEFAULT_DOOR_TOILET;
  doc["pin_bed_pressure"] = PIN_DEFAULT_BED_PRESSURE;
  doc["pin_buzzer"] = PIN_DEFAULT_BUZZER;
  // 这些传感器按数字量接入，避免在运行态对同脚位执行 analogRead 造成模式切换干扰。
  doc["analog_pir_bedroom"] = -1;
  doc["analog_pir_toilet"] = -1;
  doc["analog_door_toilet"] = -1;
  doc["analog_bed_pressure"] = -1;
  doc["risk"] = alertLevelToString(g_state.riskToday); doc["today_total_triggers"] = g_state.totalTriggersToday;
  publishMqttJson(doc, "realtime");

  g_state.lastMqttSensorPublishMs = nowMs; g_state.sensorStateChanged = false;
}
void beep(uint16_t durationMs, uint8_t repeat = 1) {
  for (uint8_t i = 0; i < repeat; ++i) {
    digitalWrite(PIN_DEFAULT_BUZZER, HIGH); delay(durationMs);
    digitalWrite(PIN_DEFAULT_BUZZER, LOW); delay(120);
  }
}

void escalateRisk(AlertLevel level) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(g_state.riskToday)) g_state.riskToday = level;
}

void resetDailyFlagsAndCounters() {
  g_state.bedroomPirTriggersToday = 0; g_state.toiletDoorChangesToday = 0; g_state.totalTriggersToday = 0;
  g_state.wakeupMinuteToday = INVALID_WAKEUP_MINUTE; g_state.wakeupWarnSent = false; g_state.toiletWarnSent = false;
  g_state.toiletCriticalSent = false; g_state.bedWarnSent = false; g_state.noActivityWarnSent = false; g_state.activityDropWarnSent = false;
  g_state.riskToday = AlertLevel::INFO;
}

void pushDayRecord(const DayRecord& record) {
  g_state.history[g_state.historyWriteIndex] = record;
  g_state.historyWriteIndex = (g_state.historyWriteIndex + 1) % HISTORY_DAYS;
  if (g_state.historyCount < HISTORY_DAYS) g_state.historyCount++;
}

void rolloverDayIfNeeded(uint32_t nowMs) {
  uint32_t currentDay = nowMs / DAY_MS;
  if (currentDay == g_state.uptimeDay) return;
  DayRecord done{}; done.totalTriggers = g_state.totalTriggersToday; done.wakeupMinute = g_state.wakeupMinuteToday;
  pushDayRecord(done); g_state.uptimeDay = currentDay; resetDailyFlagsAndCounters(); g_state.lastActivityMs = nowMs;
}

uint16_t averageTriggerCount() {
  if (g_state.historyCount == 0) return 120;
  uint32_t sum = 0; for (uint8_t i = 0; i < g_state.historyCount; ++i) sum += g_state.history[i].totalTriggers;
  return static_cast<uint16_t>(sum / g_state.historyCount);
}

uint16_t averageWakeupMinute() {
  uint32_t sum = 0; uint8_t count = 0;
  for (uint8_t i = 0; i < g_state.historyCount; ++i) if (g_state.history[i].wakeupMinute != INVALID_WAKEUP_MINUTE) { sum += g_state.history[i].wakeupMinute; count++; }
  return count == 0 ? DEFAULT_WAKEUP_MINUTE : static_cast<uint16_t>(sum / count);
}

void scanSensors(uint32_t nowMs) {
  const bool bedroomPir = digitalRead(PIN_DEFAULT_PIR_BEDROOM) == HIGH;
  const bool toiletPir = digitalRead(PIN_DEFAULT_PIR_TOILET) == HIGH;
  const bool toiletDoorClosed = digitalRead(PIN_DEFAULT_DOOR_TOILET) == HIGH;
  const int bedRaw = digitalRead(PIN_DEFAULT_BED_PRESSURE);
  const bool bedOccupied = g_config.bedActiveLow ? (bedRaw == LOW) : (bedRaw == HIGH);

  const bool changed = (bedroomPir != g_state.lastBedroomPir) || (toiletPir != g_state.lastToiletPir) ||
                       (toiletDoorClosed != g_state.lastToiletDoorClosed) || (bedOccupied != g_state.lastBedOccupied);
  if (changed) g_state.sensorStateChanged = true;

  if (bedroomPir && !g_state.lastBedroomPir) { g_state.bedroomPirTriggersToday++; g_state.totalTriggersToday++; g_state.lastActivityMs = nowMs; }
  if (toiletPir && !g_state.lastToiletPir) { g_state.totalTriggersToday++; g_state.lastActivityMs = nowMs; }
  if (toiletDoorClosed != g_state.lastToiletDoorClosed) { g_state.toiletDoorChangesToday++; g_state.totalTriggersToday++; g_state.lastActivityMs = nowMs; }

  const uint16_t minuteOfDay = minuteOfDayFromUptime(nowMs);
  if (g_state.lastBedOccupied && !bedOccupied && g_state.wakeupMinuteToday == INVALID_WAKEUP_MINUTE && minuteOfDay >= 240 && minuteOfDay <= 720) {
    g_state.wakeupMinuteToday = minuteOfDay;
  }

  if (bedOccupied && !g_state.lastBedOccupied) g_state.bedOccupiedStartMs = nowMs;
  if (!bedOccupied) {
    g_state.bedOccupiedStartMs = 0;
    g_state.bedWarnSent = false;
    if (g_config.demoMode) g_state.wakeupWarnSent = false;
  }

  if (toiletDoorClosed && toiletPir) {
    if (g_state.toiletEnterMs == 0) g_state.toiletEnterMs = nowMs;
  } else {
    g_state.toiletEnterMs = 0; g_state.toiletWarnSent = false; g_state.toiletCriticalSent = false;
  }

  g_state.bedroomPir = bedroomPir; g_state.toiletPir = toiletPir; g_state.toiletDoorClosed = toiletDoorClosed; g_state.bedOccupied = bedOccupied;
  g_state.lastBedroomPir = bedroomPir; g_state.lastToiletPir = toiletPir; g_state.lastToiletDoorClosed = toiletDoorClosed; g_state.lastBedOccupied = bedOccupied;
}

void evaluateRules(uint32_t nowMs) {
  const uint16_t minuteOfDay = minuteOfDayFromUptime(nowMs);
  const uint16_t wakeupBaseline = averageWakeupMinute();
  const uint16_t activityBaseline = averageTriggerCount();

  if (g_config.demoMode) {
    // Demo mode: if still in bed for 10s, trigger wakeup anomaly for presentation.
    if (!g_state.wakeupWarnSent && g_state.bedOccupied && g_state.bedOccupiedStartMs > 0 &&
        (nowMs - g_state.bedOccupiedStartMs > DEMO_WAKEUP_STAY_MS)) {
      g_state.wakeupWarnSent = true;
      escalateRisk(AlertLevel::WARNING);
      publishAlert("Wakeup Anomaly", "Demo mode: still in bed for over 10s.", AlertLevel::WARNING);
    }
  } else if (!g_state.wakeupWarnSent && minuteOfDay > static_cast<uint16_t>(wakeupBaseline + WAKEUP_TOLERANCE_MINUTES) &&
             g_state.bedOccupied && g_state.bedroomPirTriggersToday < 2) {
    g_state.wakeupWarnSent = true; escalateRisk(AlertLevel::WARNING); publishAlert("Wakeup Anomaly", "Wakeup later than baseline by >90 min.", AlertLevel::WARNING);
  }

  if (g_state.toiletEnterMs > 0) {
    const uint32_t stayMs = nowMs - g_state.toiletEnterMs;
    if (stayMs > toiletWarnThresholdMs() && !g_state.toiletWarnSent) { g_state.toiletWarnSent = true; escalateRisk(AlertLevel::WARNING); beep(200, 2); publishAlert("Toilet Stay Warning", "Toilet stay exceeded threshold.", AlertLevel::WARNING); }
    if (stayMs > toiletCriticalThresholdMs() && !g_state.toiletCriticalSent) { g_state.toiletCriticalSent = true; escalateRisk(AlertLevel::CRITICAL); beep(250, 4); publishAlert("Toilet Stay Critical", "Toilet stay exceeded critical threshold.", AlertLevel::CRITICAL); }
  }

  if (!g_state.bedWarnSent && isDaytimeWindow(minuteOfDay) && g_state.bedOccupied && g_state.bedOccupiedStartMs > 0 && (nowMs - g_state.bedOccupiedStartMs > bedWarnThresholdMs())) {
    g_state.bedWarnSent = true; escalateRisk(AlertLevel::WARNING); beep(200, 3); publishAlert("Long Bed Occupancy", "Daytime bed occupancy exceeded threshold.", AlertLevel::WARNING);
  }

  if (!g_state.noActivityWarnSent && !isSleepWindow(minuteOfDay) && nowMs - g_state.lastActivityMs > noActivityWarnThresholdMs()) {
    g_state.noActivityWarnSent = true; escalateRisk(AlertLevel::WARNING); beep(180, 2); publishAlert("No Activity Warning", "No obvious activity exceeded threshold.", AlertLevel::WARNING);
  }

  if (!g_state.activityDropWarnSent && minuteOfDay > 20 * 60 && g_state.totalTriggersToday < (activityBaseline / 2)) {
    g_state.activityDropWarnSent = true; escalateRisk(AlertLevel::WARNING); publishAlert("Activity Drop Warning", "Today's activity < 50% baseline.", AlertLevel::WARNING);
  }
}

void printPinSnapshot() {
  Serial.printf("PIR_BED(GPIO%d) d=%d | PIR_TOI(GPIO%d) d=%d | DOOR(GPIO%d) d=%d | BED(GPIO%d) d=%d | BUZ(GPIO%d)\n",
                PIN_DEFAULT_PIR_BEDROOM, digitalRead(PIN_DEFAULT_PIR_BEDROOM),
                PIN_DEFAULT_PIR_TOILET, digitalRead(PIN_DEFAULT_PIR_TOILET),
                PIN_DEFAULT_DOOR_TOILET, digitalRead(PIN_DEFAULT_DOOR_TOILET),
                PIN_DEFAULT_BED_PRESSURE, digitalRead(PIN_DEFAULT_BED_PRESSURE),
                PIN_DEFAULT_BUZZER);
}

void printWifiStatus() {
  Serial.printf("wifi=%d ip=%s ap=%s ap_ip=%s ssid=%s time=%s\n",
                static_cast<int>(WiFi.status()), WiFi.localIP().toString().c_str(),
                g_apConfigMode ? "on" : "off", WiFi.softAPIP().toString().c_str(),
                g_config.ssid.c_str(), formatNowTime().c_str());
}

void printSerialHelp() {
  Serial.println("help | pins | pinmap | d <pin> | a <pin> | wifi | setwifi <ssid> <pwd> | reconnect | demo on|off | time");
}
void processSerialCommand(const String& rawLine) {
  String line = rawLine; line.trim(); if (line.length() == 0) return;
  if (line == "help") { printSerialHelp(); return; }
  if (line == "pins") { printPinSnapshot(); return; }
  if (line == "wifi") { printWifiStatus(); return; }
  if (line == "time") { Serial.printf("time_synced=%s now=%s\n", g_timeSynced ? "true" : "false", formatNowTime().c_str()); return; }
  if (line == "reconnect") { stopApConfigMode(); g_wifiConnectFailCount = 0; WiFi.disconnect(true, false); g_lastWifiBeginMs = 0; ensureWifiConnected(); return; }
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
    if (isManagedSensorOrActuatorPin(pin)) applyPinModes();
    return;
  }
  if (line.startsWith("a ")) {
    int pin = line.substring(2).toInt();
    pinMode(pin, INPUT);
    Serial.printf("analogRead(GPIO%d)=%d\n", pin, safeAnalogRead(pin));
    if (isManagedSensorOrActuatorPin(pin)) applyPinModes();
    return;
  }

  if (line.startsWith("setwifi ")) {
    int firstSpace = line.indexOf(' '); int secondSpace = line.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0) { Serial.println("usage: setwifi <ssid> <pwd>"); return; }
    g_config.ssid = line.substring(firstSpace + 1, secondSpace);
    g_config.pass = line.substring(secondSpace + 1);
    g_config.ssid.trim(); g_config.pass.trim();
    if (g_config.ssid.length() == 0) { Serial.println("SSID cannot be empty."); return; }
    saveConfig(); stopApConfigMode(); g_wifiConnectFailCount = 0; WiFi.disconnect(true, false); g_lastWifiBeginMs = 0; ensureWifiConnected();
    return;
  }
  if (line == "pinmap") {
    printPinSnapshot();
    return;
  }

  Serial.printf("unknown command: %s\n", line.c_str());
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    g_lastSerialInputMs = millis();
    if (c == '\r' || c == '\n' || c == ';') {
      if (g_serialLine.length() == 0) continue;
      processSerialCommand(g_serialLine);
      g_serialLine = "";
    } else {
      g_serialLine += c;
      if (g_serialLine.length() > 180) { g_serialLine = ""; Serial.println("command too long, cleared."); }
    }
  }
  if (g_serialLine.length() > 0 && (millis() - g_lastSerialInputMs > 500)) {
    processSerialCommand(g_serialLine);
    g_serialLine = "";
  }
}

String currentStatusLabel() {
  if (g_state.riskToday == AlertLevel::CRITICAL) return "高风险";
  if (g_state.riskToday == AlertLevel::WARNING) return "中度异常";
  return "正常";
}

void handleApiStatus() {
  JsonDocument doc;
  uint32_t nowMs = millis();
  doc["status_cn"] = currentStatusLabel();
  doc["risk"] = alertLevelToString(g_state.riskToday);
  doc["risk_cn"] = alertLevelToChinese(g_state.riskToday);
  doc["minute_of_day"] = minuteOfDayFromUptime(nowMs);
  doc["wakeup_recorded"] = g_state.wakeupMinuteToday != INVALID_WAKEUP_MINUTE;
  doc["bed_occupied"] = g_state.bedOccupied;
  doc["bed_raw"] = digitalRead(PIN_DEFAULT_BED_PRESSURE);
  doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["toilet_pir"] = g_state.toiletPir;
  doc["today_total_triggers"] = g_state.totalTriggersToday;
  doc["avg_total_triggers_5d"] = averageTriggerCount();
  doc["avg_wakeup_minute_5d"] = averageWakeupMinute();
  doc["wakeup_warn"] = g_state.wakeupWarnSent;
  doc["toilet_warn"] = g_state.toiletWarnSent;
  doc["toilet_critical"] = g_state.toiletCriticalSent;
  doc["bed_warn"] = g_state.bedWarnSent;
  doc["no_activity_warn"] = g_state.noActivityWarnSent;
  doc["activity_drop_warn"] = g_state.activityDropWarnSent;
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["wifi_ssid"] = g_config.ssid;
  doc["wifi_ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0";
  doc["ap_mode"] = g_apConfigMode;
  doc["ap_ssid"] = AP_SSID;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["demo_mode"] = g_config.demoMode;
  doc["bed_active_low"] = g_config.bedActiveLow;
  doc["pin_pir_bedroom"] = PIN_DEFAULT_PIR_BEDROOM;
  doc["pin_pir_toilet"] = PIN_DEFAULT_PIR_TOILET;
  doc["pin_door_toilet"] = PIN_DEFAULT_DOOR_TOILET;
  doc["pin_bed_pressure"] = PIN_DEFAULT_BED_PRESSURE;
  doc["pin_buzzer"] = PIN_DEFAULT_BUZZER;
  doc["time_synced"] = g_timeSynced;
  doc["now_time"] = formatNowTime();
  String payload; serializeJson(doc, payload);
  g_web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  g_web.sendHeader("Pragma", "no-cache");
  g_web.sendHeader("Expires", "0");
  g_web.send(200, "application/json; charset=utf-8", payload);
}

void handleApiWifiConfig() {
  if (!g_web.hasArg("ssid")) { g_web.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"missing ssid\"}"); return; }
  g_config.ssid = g_web.arg("ssid"); g_config.pass = g_web.arg("pass");
  g_config.ssid.trim(); g_config.pass.trim();
  if (g_config.ssid.length() == 0) { g_web.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"ssid empty\"}"); return; }
  saveConfig(); stopApConfigMode(); g_wifiConnectFailCount = 0; WiFi.disconnect(true, false); g_lastWifiBeginMs = 0; ensureWifiConnected();
  g_web.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"msg\":\"saved and reconnecting\"}");
}

void handleApiDevConfig() {
  if (g_web.method() == HTTP_GET) {
    JsonDocument doc;
    doc["demo_mode"] = g_config.demoMode;
    doc["bed_active_low"] = g_config.bedActiveLow;
    doc["pin_pir_bedroom"] = PIN_DEFAULT_PIR_BEDROOM;
    doc["pin_pir_toilet"] = PIN_DEFAULT_PIR_TOILET;
    doc["pin_door_toilet"] = PIN_DEFAULT_DOOR_TOILET;
    doc["pin_bed_pressure"] = PIN_DEFAULT_BED_PRESSURE;
    doc["pin_buzzer"] = PIN_DEFAULT_BUZZER;
    String payload; serializeJson(doc, payload);
    g_web.send(200, "application/json; charset=utf-8", payload); return;
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
  if (oldDemoMode && !g_config.demoMode) {
    // Exit demo mode: clear transient warning latches so normal thresholds can take over immediately.
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

void handleCaptiveRedirect() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  g_web.sendHeader("Location", url, true);
  g_web.send(302, "text/plain", "Captive portal");
}

void handleHotspotDetect() { handleCaptiveRedirect(); }
void handleGenerate204() { handleCaptiveRedirect(); }
void handleConnectTest() { handleCaptiveRedirect(); }
void handleNcsi() { handleCaptiveRedirect(); }
String htmlPage() {
  String page;
  page.reserve(7800);
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>独居老人隐私预警系统</title>";
  page += "<style>body{font-family:Arial,Helvetica,sans-serif;background:#f6f7fb;margin:0;padding:20px;color:#1d2433;}";
  page += ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 3px 12px rgba(0,0,0,.08);} .title{font-size:20px;font-weight:700;margin-bottom:8px;cursor:pointer;}";
  page += ".risk-normal{color:#147a3d;font-weight:700;} .risk-warning{color:#a66b00;font-weight:700;} .risk-critical{color:#b5121b;font-weight:700;}";
  page += ".k{color:#5e6b82}.v{font-weight:700}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;} .hidden{display:none;} .dev{border:1px dashed #8aa3cc;background:#f0f5ff;}";
  page += "@media(max-width:760px){.grid{grid-template-columns:1fr;}}</style></head><body>";

  page += "<div class='card'><div id='appTitle' class='title'>独居老人隐私预警系统</div><div id='riskText' class='risk-normal'>加载中...</div><div><span class='k'>当前时间</span><div id='nowTime' class='v'>-</div></div></div>";
  page += "<div class='card'><div class='title' style='cursor:default;'>接线图（程序内固定常量）</div>";
  page += "<div>卧室 PIR: GPIO <span id='pinBedPir'>-</span></div>";
  page += "<div>厕所 PIR: GPIO <span id='pinToiPir'>-</span></div>";
  page += "<div>厕所门磁: GPIO <span id='pinDoor'>-</span></div>";
  page += "<div>床压传感器: GPIO <span id='pinBed'>-</span></div>";
  page += "<div>蜂鸣器: GPIO <span id='pinBuz'>-</span></div></div>";
  page += "<div class='card grid'>";
  page += "<div><span class='k'>今日活动触发</span><div class='v' id='total'>-</div></div>";
  page += "<div><span class='k'>5日平均触发</span><div class='v' id='avg'>-</div></div>";
  page += "<div><span class='k'>卧床状态</span><div class='v' id='bed'>-</div></div>";
  page += "<div><span class='k'>床压原始电平</span><div class='v' id='bedRaw'>-</div></div>";
  page += "<div><span class='k'>厕所状态</span><div class='v' id='toilet'>-</div></div>";
  page += "<div><span class='k'>起床异常</span><div class='v' id='wakeup'>-</div></div>";
  page += "<div><span class='k'>活动量骤降</span><div class='v' id='drop'>-</div></div>";
  page += "</div>";

  page += "<div class='card'><div><span class='k'>联网状态</span><div class='v' id='net'>-</div></div><div><span class='k'>设备 IP</span><div class='v' id='ip'>-</div></div>";
  page += "<div id='apTip' style='display:none;margin-top:10px;color:#a66b00;'>设备当前未联网，已开启 AP 配网：<b id='apSsid'></b>（IP: <span id='apIp'></span>）</div>";
  page += "<div id='cfgForm' style='display:none;margin-top:10px;'><input id='ssid' placeholder='Wi-Fi SSID' style='padding:8px;margin-right:6px;'><input id='pass' type='password' placeholder='Wi-Fi 密码' style='padding:8px;margin-right:6px;'><button onclick='saveWifi()'>提交配网</button></div></div>";

  page += "<div id='devPanel' class='card dev hidden'><div class='title' style='cursor:default;'>开发者选项</div>";
  page += "<label><input type='checkbox' id='demoMode'> 演示模式（赖床10秒告警，厕所阈值1秒；关闭后恢复）</label>";
  page += "<div style='margin-top:8px;color:#5e6b82;'>引脚已在程序中固定，如需改线请修改文件顶部常量后重新烧录。</div>";
  page += "<div><label><input type='checkbox' id='bedActiveLow'> 床压低电平=在床（未勾选则高电平=在床）</label></div>";
  page += "<div style='margin-top:10px;'><button onclick='saveDev()'>保存开发者配置</button></div></div>";

  page += "<script>let titleClick=0,lastClickAt=0,devVisible=false;";
  page += "async function refreshDevControls(){try{const r=await fetch('/api/dev-config?t='+Date.now(),{cache:'no-store'});const d=await r.json();document.getElementById('demoMode').checked=!!d.demo_mode;document.getElementById('bedActiveLow').checked=(d.bed_active_low!==false);}catch(e){}}";
  page += "document.getElementById('appTitle').addEventListener('click',()=>{const now=Date.now();if(now-lastClickAt>1500)titleClick=0;titleClick++;lastClickAt=now;if(titleClick>=5){devVisible=!devVisible;document.getElementById('devPanel').classList.toggle('hidden',!devVisible);refreshDevControls();titleClick=0;}});";
  page += "async function pull(){try{const r=await fetch('/api/status?t='+Date.now(),{cache:'no-store'});const d=await r.json();const risk=document.getElementById('riskText');risk.textContent='风险等级：'+d.risk_cn+'（状态：'+d.status_cn+'）';risk.className=d.risk==='critical'?'risk-critical':(d.risk==='warning'?'risk-warning':'risk-normal');document.getElementById('nowTime').textContent=d.now_time;document.getElementById('total').textContent=d.today_total_triggers;document.getElementById('avg').textContent=d.avg_total_triggers_5d;document.getElementById('bed').textContent=d.bed_occupied?'床上有人':'已离床';document.getElementById('bedRaw').textContent=d.bed_raw;document.getElementById('toilet').textContent=(d.toilet_door_closed&&d.toilet_pir)?'疑似滞留':'正常';document.getElementById('wakeup').textContent=d.wakeup_warn?'异常':'正常';document.getElementById('drop').textContent=d.activity_drop_warn?'异常':'正常';document.getElementById('net').textContent=d.wifi_connected?('已联网：'+d.wifi_ssid):'未联网（AP 配网中）';document.getElementById('ip').textContent=d.wifi_ip;document.getElementById('pinBedPir').textContent=d.pin_pir_bedroom;document.getElementById('pinToiPir').textContent=d.pin_pir_toilet;document.getElementById('pinDoor').textContent=d.pin_door_toilet;document.getElementById('pinBed').textContent=d.pin_bed_pressure;document.getElementById('pinBuz').textContent=d.pin_buzzer;const showCfg=!d.wifi_connected;document.getElementById('cfgForm').style.display=showCfg?'block':'none';document.getElementById('apTip').style.display=showCfg?'block':'none';document.getElementById('apSsid').textContent=d.ap_ssid;document.getElementById('apIp').textContent=d.ap_ip;}catch(e){document.getElementById('net').textContent='状态拉取失败';}}";
  page += "async function saveWifi(){const ssid=document.getElementById('ssid').value;const pass=document.getElementById('pass').value;if(!ssid){alert('SSID 不能为空');return;}const b='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);const r=await fetch('/api/wifi-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const d=await r.json();alert(d.ok?'已提交配网，设备正在重连':'提交失败：'+(d.msg||'unknown'));}";
  page += "async function saveDev(){const demo=document.getElementById('demoMode').checked?'1':'0';const bedActiveLow=document.getElementById('bedActiveLow').checked?'1':'0';const b='demo_mode='+demo+'&bed_active_low='+bedActiveLow;const r=await fetch('/api/dev-config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const d=await r.json();alert(d.ok?'开发者配置已保存':'保存失败');}";
  page += "pull();setInterval(pull,1000);</script></body></html>";
  return page;
}

void setupWebServer() {
  g_web.on("/", HTTP_GET, []() {
    g_web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    g_web.sendHeader("Pragma", "no-cache");
    g_web.sendHeader("Expires", "0");
    g_web.send(200, "text/html; charset=utf-8", htmlPage());
  });
  g_web.on("/api/status", HTTP_GET, handleApiStatus);
  g_web.on("/api/wifi-config", HTTP_POST, handleApiWifiConfig);
  g_web.on("/api/dev-config", HTTP_GET, handleApiDevConfig);
  g_web.on("/api/dev-config", HTTP_POST, handleApiDevConfig);
  g_web.on("/generate_204", HTTP_GET, handleGenerate204);
  g_web.on("/gen_204", HTTP_GET, handleGenerate204);
  g_web.on("/hotspot-detect.html", HTTP_GET, handleHotspotDetect);
  g_web.on("/library/test/success.html", HTTP_GET, handleHotspotDetect);
  g_web.on("/fwlink", HTTP_GET, handleConnectTest);
  g_web.on("/connecttest.txt", HTTP_GET, handleConnectTest);
  g_web.on("/ncsi.txt", HTTP_GET, handleNcsi);
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
  Serial.println("[INFO] Commands: help/pins/d/a/wifi/setwifi/reconnect/demo/time");

  SPIFFS.begin(true);
  loadConfig();

  applyPinModes();

  uint32_t nowMs = millis();
  g_state.uptimeDay = nowMs / DAY_MS;
  g_state.lastActivityMs = nowMs;

  ensureWifiConnected();
  setupWebServer();
  g_mqtt.setServer(MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  ensureMqttConnected(nowMs);
  updateTimeSyncStatus(nowMs);
}

void loop() {
  uint32_t nowMs = millis();
  rolloverDayIfNeeded(nowMs);
  ensureWifiConnected();
  updateTimeSyncStatus(nowMs);
  scanSensors(nowMs);

  ensureMqttConnected(nowMs);
  g_mqtt.loop();
  if (g_dnsRunning) {
    g_dns.processNextRequest();
  }

  g_web.handleClient();
  handleSerialCommands();
  publishSensorRealtimeIfNeeded(nowMs);
  publishHeartbeatIfNeeded(nowMs);
  evaluateRules(nowMs);

  delay(50);
}
