#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Pin map
constexpr uint8_t PIN_PIR_BEDROOM = 14;
constexpr uint8_t PIN_PIR_TOILET = 27;
constexpr uint8_t PIN_DOOR_TOILET = 26;
constexpr uint8_t PIN_BED_PRESSURE = 25;
constexpr uint8_t PIN_BUZZER = 33;

// Wi-Fi and MQTT
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* MQTT_BROKER_ADDR = "broker.example.com";
constexpr uint16_t MQTT_BROKER_PORT = 1883;
const char* MQTT_CLIENT_ID = "senior-alert-esp32";
const char* MQTT_USERNAME = "senior_alert";
const char* MQTT_PRIVATE_KEY = "YOUR_MQTT_PRIVATE_KEY";
const char* MQTT_ALERT_TOPIC = "senior/alert/events";

// Rule thresholds
constexpr uint32_t MINUTE_MS = 60UL * 1000UL;
constexpr uint32_t HOUR_MS = 60UL * MINUTE_MS;
constexpr uint32_t DAY_MS = 24UL * HOUR_MS;

constexpr uint32_t TOILET_WARN_MS = 12UL * MINUTE_MS;
constexpr uint32_t TOILET_CRITICAL_MS = 18UL * MINUTE_MS;
constexpr uint32_t BED_WARN_MS = 3UL * HOUR_MS;
constexpr uint32_t NO_ACTIVITY_WARN_MS = 2UL * HOUR_MS;

constexpr uint16_t DEFAULT_WAKEUP_MINUTE = 7 * 60;  // 07:00
constexpr uint16_t WAKEUP_TOLERANCE_MINUTES = 90;

constexpr uint8_t HISTORY_DAYS = 5;
constexpr uint16_t INVALID_WAKEUP_MINUTE = 0xFFFF;

WiFiClient g_wifiClient;
PubSubClient g_mqtt(g_wifiClient);
WebServer g_web(80);

enum class AlertLevel : uint8_t {
  INFO = 0,
  WARNING = 1,
  CRITICAL = 2
};

struct DayRecord {
  uint16_t totalTriggers = 0;
  uint16_t wakeupMinute = INVALID_WAKEUP_MINUTE;
};

struct RuntimeState {
  // Current sensor state
  bool bedroomPir = false;
  bool toiletPir = false;
  bool toiletDoorClosed = false;
  bool bedOccupied = false;

  // Edge detection cache
  bool lastBedroomPir = false;
  bool lastToiletPir = false;
  bool lastToiletDoorClosed = false;
  bool lastBedOccupied = false;

  // Time markers
  uint32_t lastActivityMs = 0;
  uint32_t bedOccupiedStartMs = 0;
  uint32_t toiletEnterMs = 0;
  uint32_t lastDashboardMs = 0;
  uint32_t lastMqttRetryMs = 0;

  // Daily counters
  uint32_t uptimeDay = 0;
  uint16_t bedroomPirTriggersToday = 0;
  uint16_t toiletDoorChangesToday = 0;
  uint16_t totalTriggersToday = 0;
  uint16_t wakeupMinuteToday = INVALID_WAKEUP_MINUTE;

  // Rule one-shot guards per day
  bool wakeupWarnSent = false;
  bool toiletWarnSent = false;
  bool toiletCriticalSent = false;
  bool bedWarnSent = false;
  bool noActivityWarnSent = false;
  bool activityDropWarnSent = false;

  // Highest alert reached today
  AlertLevel riskToday = AlertLevel::INFO;

  // Rolling history
  DayRecord history[HISTORY_DAYS];
  uint8_t historyWriteIndex = 0;
  uint8_t historyCount = 0;
};

RuntimeState g_state;

const char* alertLevelToString(AlertLevel level) {
  switch (level) {
    case AlertLevel::INFO:
      return "normal";
    case AlertLevel::WARNING:
      return "warning";
    case AlertLevel::CRITICAL:
      return "critical";
    default:
      return "unknown";
  }
}

const char* alertLevelToChinese(AlertLevel level) {
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

uint16_t minuteOfDayFromUptime(uint32_t nowMs) {
  return static_cast<uint16_t>((nowMs / MINUTE_MS) % (24UL * 60UL));
}

bool isSleepWindow(uint16_t minuteOfDay) {
  return (minuteOfDay >= 23 * 60) || (minuteOfDay <= 6 * 60);
}

bool isDaytimeWindow(uint16_t minuteOfDay) {
  return minuteOfDay >= 8 * 60 && minuteOfDay <= 21 * 60;
}

void beep(uint16_t durationMs, uint8_t repeat = 1) {
  for (uint8_t i = 0; i < repeat; ++i) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(PIN_BUZZER, LOW);
    delay(120);
  }
}

void escalateRisk(AlertLevel level) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(g_state.riskToday)) {
    g_state.riskToday = level;
  }
}

void resetDailyFlagsAndCounters() {
  g_state.bedroomPirTriggersToday = 0;
  g_state.toiletDoorChangesToday = 0;
  g_state.totalTriggersToday = 0;
  g_state.wakeupMinuteToday = INVALID_WAKEUP_MINUTE;

  g_state.wakeupWarnSent = false;
  g_state.toiletWarnSent = false;
  g_state.toiletCriticalSent = false;
  g_state.bedWarnSent = false;
  g_state.noActivityWarnSent = false;
  g_state.activityDropWarnSent = false;

  g_state.riskToday = AlertLevel::INFO;
}

void pushDayRecord(const DayRecord& record) {
  g_state.history[g_state.historyWriteIndex] = record;
  g_state.historyWriteIndex = (g_state.historyWriteIndex + 1) % HISTORY_DAYS;
  if (g_state.historyCount < HISTORY_DAYS) {
    g_state.historyCount++;
  }
}

void rolloverDayIfNeeded(uint32_t nowMs) {
  uint32_t currentDay = nowMs / DAY_MS;
  if (currentDay == g_state.uptimeDay) {
    return;
  }

  DayRecord done{};
  done.totalTriggers = g_state.totalTriggersToday;
  done.wakeupMinute = g_state.wakeupMinuteToday;
  pushDayRecord(done);

  g_state.uptimeDay = currentDay;
  resetDailyFlagsAndCounters();
  g_state.lastActivityMs = nowMs;

  Serial.println("[INFO] New day started, daily counters reset.");
}

uint16_t averageTriggerCount() {
  if (g_state.historyCount == 0) {
    return 120;  // bootstrap baseline
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
    if (g_state.history[i].wakeupMinute != INVALID_WAKEUP_MINUTE) {
      sum += g_state.history[i].wakeupMinute;
      count++;
    }
  }
  if (count == 0) {
    return DEFAULT_WAKEUP_MINUTE;
  }
  return static_cast<uint16_t>(sum / count);
}

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[INFO] Connecting Wi-Fi: %s\n", WIFI_SSID);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[INFO] Wi-Fi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WARN] Wi-Fi not connected, continue offline.");
  }
}

void ensureMqttConnected(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED || g_mqtt.connected()) {
    return;
  }

  if (nowMs - g_state.lastMqttRetryMs < 5000) {
    return;
  }
  g_state.lastMqttRetryMs = nowMs;

  g_mqtt.setServer(MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  Serial.printf("[INFO] Connecting MQTT: %s:%u\n", MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  if (g_mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PRIVATE_KEY)) {
    Serial.println("[INFO] MQTT connected.");
  } else {
    Serial.printf("[WARN] MQTT connect failed, state=%d\n", g_mqtt.state());
  }
}

void publishAlert(const char* title, const char* detail, AlertLevel level) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] Skip MQTT alert: Wi-Fi disconnected.");
    return;
  }

  ensureMqttConnected(millis());
  if (!g_mqtt.connected()) {
    Serial.println("[WARN] Skip MQTT alert: MQTT disconnected.");
    return;
  }

  JsonDocument doc;
  doc["title"] = title;
  doc["detail"] = detail;
  doc["risk"] = alertLevelToString(level);
  doc["risk_cn"] = alertLevelToChinese(level);
  doc["uptime_ms"] = millis();
  doc["minute_of_day"] = minuteOfDayFromUptime(millis());
  doc["bed_occupied"] = g_state.bedOccupied;
  doc["toilet_door_closed"] = g_state.toiletDoorClosed;
  doc["toilet_pir"] = g_state.toiletPir;
  doc["today_total_triggers"] = g_state.totalTriggersToday;

  String payload;
  serializeJson(doc, payload);
  bool ok = g_mqtt.publish(MQTT_ALERT_TOPIC, payload.c_str(), true);
  Serial.printf("[INFO] MQTT publish %s: %s\n", ok ? "success" : "failed", title);
}

void scanSensors(uint32_t nowMs) {
  // Adjust logic if your sensors are active-low.
  const bool bedroomPir = digitalRead(PIN_PIR_BEDROOM) == HIGH;
  const bool toiletPir = digitalRead(PIN_PIR_TOILET) == HIGH;
  const bool toiletDoorClosed = digitalRead(PIN_DOOR_TOILET) == HIGH;
  const bool bedOccupied = digitalRead(PIN_BED_PRESSURE) == HIGH;

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

  // First "get out of bed" event in morning window is today's wakeup time.
  const uint16_t minuteOfDay = minuteOfDayFromUptime(nowMs);
  if (g_state.lastBedOccupied && !bedOccupied &&
      g_state.wakeupMinuteToday == INVALID_WAKEUP_MINUTE &&
      minuteOfDay >= 4 * 60 && minuteOfDay <= 12 * 60) {
    g_state.wakeupMinuteToday = minuteOfDay;
  }

  if (bedOccupied && !g_state.lastBedOccupied) {
    g_state.bedOccupiedStartMs = nowMs;
  }
  if (!bedOccupied) {
    g_state.bedOccupiedStartMs = 0;
    g_state.bedWarnSent = false;
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

void evaluateRules(uint32_t nowMs) {
  const uint16_t minuteOfDay = minuteOfDayFromUptime(nowMs);
  const uint16_t wakeupBaseline = averageWakeupMinute();
  const uint16_t activityBaseline = averageTriggerCount();

  // Rule A: Wakeup anomaly
  if (!g_state.wakeupWarnSent &&
      minuteOfDay > static_cast<uint16_t>(wakeupBaseline + WAKEUP_TOLERANCE_MINUTES) &&
      g_state.bedOccupied &&
      g_state.bedroomPirTriggersToday < 2) {
    g_state.wakeupWarnSent = true;
    escalateRisk(AlertLevel::WARNING);
    Serial.println("[ALERT] Wakeup anomaly: still in bed and low bedroom activity.");
    publishAlert("Wakeup Anomaly", "Wakeup is later than baseline by over 90 minutes.", AlertLevel::WARNING);
  }

  // Rule B: Toilet stay too long (12 min warning, 18 min critical)
  if (g_state.toiletEnterMs > 0) {
    const uint32_t stayMs = nowMs - g_state.toiletEnterMs;

    if (stayMs > TOILET_WARN_MS && !g_state.toiletWarnSent) {
      g_state.toiletWarnSent = true;
      escalateRisk(AlertLevel::WARNING);
      beep(200, 2);
      Serial.println("[ALERT] Toilet stay exceeded 12 minutes (local warning).");
      publishAlert("Toilet Stay Warning", "Toilet stay exceeded 12 minutes.", AlertLevel::WARNING);
    }

    if (stayMs > TOILET_CRITICAL_MS && !g_state.toiletCriticalSent) {
      g_state.toiletCriticalSent = true;
      escalateRisk(AlertLevel::CRITICAL);
      beep(300, 4);
      Serial.println("[ALERT] Toilet stay exceeded 18 minutes (critical).");
      publishAlert("Toilet Stay Critical", "Toilet stay exceeded 18 minutes.", AlertLevel::CRITICAL);
    }
  }

  // Rule C: Long daytime bed occupancy
  if (!g_state.bedWarnSent && isDaytimeWindow(minuteOfDay) && g_state.bedOccupied && g_state.bedOccupiedStartMs > 0) {
    const uint32_t bedDurationMs = nowMs - g_state.bedOccupiedStartMs;
    if (bedDurationMs > BED_WARN_MS) {
      g_state.bedWarnSent = true;
      escalateRisk(AlertLevel::WARNING);
      beep(200, 3);
      Serial.println("[ALERT] Bed occupied over 3 hours in daytime.");
      publishAlert("Long Bed Occupancy", "Daytime bed occupancy exceeded 3 hours.", AlertLevel::WARNING);
    }
  }

  // Rule D: Long no-activity window (exclude sleep time)
  if (!g_state.noActivityWarnSent && !isSleepWindow(minuteOfDay) &&
      (nowMs - g_state.lastActivityMs > NO_ACTIVITY_WARN_MS)) {
    g_state.noActivityWarnSent = true;
    escalateRisk(AlertLevel::WARNING);
    beep(180, 2);
    Serial.println("[ALERT] No obvious activity for over 2 hours.");
    publishAlert("No Activity Warning", "No obvious activity for over 2 hours.", AlertLevel::WARNING);
  }

  // Rule E: Daily activity drop (check after 20:00)
  if (!g_state.activityDropWarnSent && minuteOfDay > 20 * 60 &&
      g_state.totalTriggersToday < (activityBaseline / 2)) {
    g_state.activityDropWarnSent = true;
    escalateRisk(AlertLevel::WARNING);
    Serial.println("[ALERT] Daily activity dropped below 50% of baseline.");
    publishAlert("Activity Drop Warning", "Today's activity is below 50% of baseline.", AlertLevel::WARNING);
  }
}

String currentStatusLabel() {
  if (g_state.riskToday == AlertLevel::CRITICAL) {
    return "高风险";
  }
  if (g_state.riskToday == AlertLevel::WARNING) {
    return "中度异常";
  }
  return "正常";
}

void printSerialDashboard(uint32_t nowMs) {
  if (nowMs - g_state.lastDashboardMs < 5000) {
    return;
  }
  g_state.lastDashboardMs = nowMs;

  const uint16_t minuteOfDay = minuteOfDayFromUptime(nowMs);
  Serial.println("=============== Senior Status Dashboard ===============");
  Serial.printf("Risk today: %s\n", alertLevelToChinese(g_state.riskToday));
  Serial.printf("Current minute of day: %u\n", minuteOfDay);
  Serial.printf("Wakeup today: %s\n", g_state.wakeupMinuteToday == INVALID_WAKEUP_MINUTE ? "N/A" : "Recorded");
  Serial.printf("Bed state: %s\n", g_state.bedOccupied ? "occupied" : "empty");
  Serial.printf("Toilet state: %s\n", (g_state.toiletDoorClosed && g_state.toiletPir) ? "staying" : "normal");
  Serial.printf("Today's trigger count: %u\n", g_state.totalTriggersToday);
  Serial.println("=======================================================");
}

void handleApiStatus() {
  JsonDocument doc;
  const uint32_t nowMs = millis();
  const uint16_t nowMinute = minuteOfDayFromUptime(nowMs);

  doc["status_cn"] = currentStatusLabel();
  doc["risk"] = alertLevelToString(g_state.riskToday);
  doc["risk_cn"] = alertLevelToChinese(g_state.riskToday);
  doc["minute_of_day"] = nowMinute;
  doc["wakeup_recorded"] = g_state.wakeupMinuteToday != INVALID_WAKEUP_MINUTE;
  doc["bed_occupied"] = g_state.bedOccupied;
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

  String payload;
  serializeJson(doc, payload);
  g_web.send(200, "application/json; charset=utf-8", payload);
}

String htmlPage() {
  String page;
  page.reserve(2800);
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Privacy Alert Dashboard</title>";
  page += "<style>";
  page += "body{font-family:Arial,Helvetica,sans-serif;background:#f6f7fb;margin:0;padding:20px;color:#1d2433;}";
  page += ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 3px 12px rgba(0,0,0,.08);}";
  page += ".title{font-size:20px;font-weight:700;margin-bottom:8px;}";
  page += ".risk-normal{color:#147a3d;font-weight:700;}.risk-warning{color:#a66b00;font-weight:700;}.risk-critical{color:#b5121b;font-weight:700;}";
  page += ".k{color:#5e6b82}.v{font-weight:700}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
  page += "@media(max-width:760px){.grid{grid-template-columns:1fr;}}";
  page += "</style></head><body>";
  page += "<div class='card'><div class='title'>独居老人隐私预警系统</div>";
  page += "<div id='riskText' class='risk-normal'>加载中...</div></div>";
  page += "<div class='card grid'>";
  page += "<div><span class='k'>今日活动触发</span><div class='v' id='total'>-</div></div>";
  page += "<div><span class='k'>5日平均触发</span><div class='v' id='avg'>-</div></div>";
  page += "<div><span class='k'>卧床状态</span><div class='v' id='bed'>-</div></div>";
  page += "<div><span class='k'>厕所状态</span><div class='v' id='toilet'>-</div></div>";
  page += "<div><span class='k'>起床异常</span><div class='v' id='wakeup'>-</div></div>";
  page += "<div><span class='k'>活动量骤降</span><div class='v' id='drop'>-</div></div>";
  page += "</div>";
  page += "<script>";
  page += "async function pull(){";
  page += "const r=await fetch('/api/status');const d=await r.json();";
  page += "const risk=document.getElementById('riskText');";
  page += "risk.textContent='风险等级：'+d.risk_cn+'（状态：'+d.status_cn+'）';";
  page += "risk.className=d.risk==='critical'?'risk-critical':(d.risk==='warning'?'risk-warning':'risk-normal');";
  page += "document.getElementById('total').textContent=d.today_total_triggers;";
  page += "document.getElementById('avg').textContent=d.avg_total_triggers_5d;";
  page += "document.getElementById('bed').textContent=d.bed_occupied?'床上有人':'已离床';";
  page += "document.getElementById('toilet').textContent=(d.toilet_door_closed&&d.toilet_pir)?'疑似滞留':'正常';";
  page += "document.getElementById('wakeup').textContent=d.wakeup_warn?'异常':'正常';";
  page += "document.getElementById('drop').textContent=d.activity_drop_warn?'异常':'正常';";
  page += "} pull(); setInterval(pull,3000);";
  page += "</script></body></html>";
  return page;
}

void handleRoot() {
  g_web.send(200, "text/html; charset=utf-8", htmlPage());
}

void setupWebServer() {
  g_web.on("/", HTTP_GET, handleRoot);
  g_web.on("/api/status", HTTP_GET, handleApiStatus);
  g_web.begin();
  Serial.println("[INFO] Web dashboard started: http://<esp32-ip>/");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR_BEDROOM, INPUT);
  pinMode(PIN_PIR_TOILET, INPUT);
  pinMode(PIN_DOOR_TOILET, INPUT_PULLUP);
  pinMode(PIN_BED_PRESSURE, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  const uint32_t nowMs = millis();
  g_state.uptimeDay = nowMs / DAY_MS;
  g_state.lastActivityMs = nowMs;

  ensureWifiConnected();
  setupWebServer();
  g_mqtt.setServer(MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  ensureMqttConnected(nowMs);

  Serial.println("[INFO] Privacy Alert system started.");
}

void loop() {
  const uint32_t nowMs = millis();

  rolloverDayIfNeeded(nowMs);
  ensureWifiConnected();
  ensureMqttConnected(nowMs);
  g_mqtt.loop();
  g_web.handleClient();

  scanSensors(nowMs);
  evaluateRules(nowMs);
  printSerialDashboard(nowMs);

  delay(100);
}
