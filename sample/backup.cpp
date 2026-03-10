#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

/**
 * 隐私安全独居老人告警系统（ESP32 + PlatformIO）
 *
 * 传感器引脚定义（请按接线修改）：
 * - 卧室 PIR 红外       -> GPIO 14（数字输入）
 * - 厕所 PIR 红外       -> GPIO 27（数字输入）
 * - 厕所门磁传感器       -> GPIO 26（数字输入，默认 HIGH=关门，LOW=开门）
 * - 床边压力垫/压感开关   -> GPIO 25（数字输入，默认 HIGH=有人在床上）
 * - 蜂鸣器               -> GPIO 33（数字输出）
 */

// ===================== 引脚区（Pin Map） =====================
constexpr uint8_t PIN_PIR_BEDROOM = 14;   // 卧室 PIR
constexpr uint8_t PIN_PIR_TOILET = 27;    // 厕所 PIR
constexpr uint8_t PIN_DOOR_TOILET = 26;   // 厕所门磁
constexpr uint8_t PIN_BED_PRESSURE = 25;  // 床边压力垫
constexpr uint8_t PIN_BUZZER = 33;        // 蜂鸣器

// ===================== Wi-Fi / MQTT 告警配置 =====================
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// MQTT 地址与私钥（按要求设置为常量）
const char *MQTT_BROKER_ADDR = "broker.example.com";
constexpr uint16_t MQTT_BROKER_PORT = 1883;
const char *MQTT_CLIENT_ID = "senior-alert-esp32";
const char *MQTT_USERNAME = "senior_alert";
const char *MQTT_PRIVATE_KEY = "YOUR_MQTT_PRIVATE_KEY";  // 私钥/密码常量
const char *MQTT_ALERT_TOPIC = "senior/alert/events";

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ===================== 时间阈值（毫秒） =====================
constexpr uint32_t MINUTE = 60UL * 1000UL;
constexpr uint32_t HOUR = 60UL * MINUTE;
constexpr uint32_t TOILET_WARN_MS = 12UL * MINUTE;      // 厕所滞留一级提醒
constexpr uint32_t TOILET_CRITICAL_MS = 18UL * MINUTE;  // 厕所滞留二级远程告警
constexpr uint32_t BED_WARN_MS = 3UL * HOUR;            // 白天连续卧床预警
constexpr uint32_t NO_ACTIVITY_WARN_MS = 2UL * HOUR;    // 长时间无活动提醒

// 起床基准时间（示例：7:00，允许晚 90 分钟）
constexpr uint16_t AVG_WAKEUP_MINUTE = 7 * 60;
constexpr uint16_t WAKEUP_DELAY_TOLERANCE = 90;

enum class RiskLevel : uint8_t {
  NORMAL,
  LIGHT,
  MEDIUM,
  HI
};

struct RuntimeStats {
  bool bedroomPirTriggeredToday = false;
  bool toiletPirState = false;
  bool toiletDoorClosed = false;
  bool bedOccupied = false;

  uint32_t bedroomPirTriggersToday = 0;
  uint32_t toiletDoorTriggersToday = 0;
  uint32_t totalTriggersToday = 0;

  uint32_t lastActivityMs = 0;
  uint32_t toiletEnterMs = 0;
  uint32_t bedOccupiedStartMs = 0;

  bool toiletWarnSent = false;
  bool toiletCriticalSent = false;
  bool bedWarnSent = false;
  bool noActivityWarnSent = false;
  bool wakeupWarnSent = false;

  RiskLevel currentRisk = RiskLevel::NORMAL;
};

RuntimeStats stats;

// 活动量基线（示例值，可改为 EEPROM/NVS 持久化）
uint16_t avgTriggersHistory5Days = 120;

bool lastBedroomPir = false;
bool lastToiletPir = false;
bool lastToiletDoorClosed = false;
bool lastBedOccupied = false;

uint32_t lastReportMs = 0;

static const char *riskToString(RiskLevel risk) {
  switch (risk) {
    case RiskLevel::NORMAL: return "正常";
    case RiskLevel::LIGHT: return "轻度异常";
    case RiskLevel::MEDIUM: return "中度异常";
    case RiskLevel::HI: return "高风险";
  }
  return "未知";
}

void beep(uint16_t durationMs, uint8_t repeat = 1) {
  for (uint8_t i = 0; i < repeat; ++i) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(PIN_BUZZER, LOW);
    delay(120);
  }
}

void updateRisk(RiskLevel level) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(stats.currentRisk)) {
    stats.currentRisk = level;
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[INFO] 正在连接 Wi-Fi: %s\n", WIFI_SSID);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[INFO] Wi-Fi 已连接，IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WARN] Wi-Fi 连接失败，系统继续离线运行");
  }
}

void connectMqtt() {
  mqttClient.setServer(MQTT_BROKER_ADDR, MQTT_BROKER_PORT);

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqttClient.connected()) {
    return;
  }

  Serial.printf("[INFO] 正在连接 MQTT: %s:%u\n", MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PRIVATE_KEY)) {
    Serial.println("[INFO] MQTT 连接成功");
  } else {
    Serial.printf("[WARN] MQTT 连接失败, state=%d\n", mqttClient.state());
  }
}

void sendRemoteAlert(const char *title, const char *detail, RiskLevel level) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] Wi-Fi 未连接，跳过 MQTT 告警发送");
    return;
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }

  if (!mqttClient.connected()) {
    Serial.println("[WARN] MQTT 未连接，告警发送失败");
    return;
  }

  JsonDocument doc;
  doc["title"] = title;
  doc["detail"] = detail;
  doc["risk"] = riskToString(level);
  doc["uptime_ms"] = millis();
  doc["bed_occupied"] = stats.bedOccupied;
  doc["toilet_door_closed"] = stats.toiletDoorClosed;
  doc["toilet_pir"] = stats.toiletPirState;
  doc["today_total_triggers"] = stats.totalTriggersToday;

  String payload;
  serializeJson(doc, payload);

  bool ok = mqttClient.publish(MQTT_ALERT_TOPIC, payload.c_str(), true);
  Serial.printf("[INFO] MQTT 告警发送: %s\n", ok ? "成功" : "失败");
}

void scanSensors() {
  // 按照接线定义读取，若你的模块逻辑反相请改成 !digitalRead(...)
  bool bedroomPir = digitalRead(PIN_PIR_BEDROOM) == HIGH;
  bool toiletPir = digitalRead(PIN_PIR_TOILET) == HIGH;
  bool toiletDoorClosed = digitalRead(PIN_DOOR_TOILET) == HIGH;
  bool bedOccupied = digitalRead(PIN_BED_PRESSURE) == HIGH;

  uint32_t now = millis();

  if (bedroomPir && !lastBedroomPir) {
    stats.bedroomPirTriggersToday++;
    stats.totalTriggersToday++;
    stats.lastActivityMs = now;
    stats.bedroomPirTriggeredToday = true;
  }

  if (toiletPir && !lastToiletPir) {
    stats.totalTriggersToday++;
    stats.lastActivityMs = now;
  }

  if (toiletDoorClosed != lastToiletDoorClosed) {
    stats.toiletDoorTriggersToday++;
    stats.totalTriggersToday++;
    stats.lastActivityMs = now;
  }

  if (bedOccupied && !lastBedOccupied) {
    stats.bedOccupiedStartMs = now;
  }

  if (!bedOccupied) {
    stats.bedOccupiedStartMs = 0;
    stats.bedWarnSent = false;
  }

  if (toiletDoorClosed && toiletPir) {
    if (stats.toiletEnterMs == 0) {
      stats.toiletEnterMs = now;
    }
  } else {
    stats.toiletEnterMs = 0;
    stats.toiletWarnSent = false;
    stats.toiletCriticalSent = false;
  }

  stats.toiletPirState = toiletPir;
  stats.toiletDoorClosed = toiletDoorClosed;
  stats.bedOccupied = bedOccupied;

  lastBedroomPir = bedroomPir;
  lastToiletPir = toiletPir;
  lastToiletDoorClosed = toiletDoorClosed;
  lastBedOccupied = bedOccupied;
}

void evaluateRules() {
  uint32_t now = millis();

  // 规则 2：厕所滞留
  if (stats.toiletEnterMs > 0) {
    uint32_t stay = now - stats.toiletEnterMs;
    if (stay > TOILET_WARN_MS && !stats.toiletWarnSent) {
      updateRisk(RiskLevel::MEDIUM);
      beep(200, 2);
      stats.toiletWarnSent = true;
      Serial.println("[ALERT] 厕所滞留超过 12 分钟，已本地提醒");
    }
    if (stay > TOILET_CRITICAL_MS && !stats.toiletCriticalSent) {
      updateRisk(RiskLevel::HI);
      beep(300, 4);
      sendRemoteAlert("厕所滞留高风险", "门关闭且持续有人，超过 18 分钟", RiskLevel::HI);
      stats.toiletCriticalSent = true;
      Serial.println("[ALERT] 厕所滞留超过 18 分钟，已远程告警");
    }
  }

  // 规则 1：起床异常（示例按开机以来分钟模拟当天时间）
  uint32_t minuteOfDay = (now / MINUTE) % (24 * 60);
  bool wakeupLate = minuteOfDay > (AVG_WAKEUP_MINUTE + WAKEUP_DELAY_TOLERANCE);
  if (wakeupLate && stats.bedOccupied && stats.bedroomPirTriggersToday < 2 && !stats.wakeupWarnSent) {
    updateRisk(RiskLevel::LIGHT);
    stats.wakeupWarnSent = true;
    Serial.println("[ALERT] 起床异常：晚于基线且仍卧床、活动少");
  }

  // 规则 3：活动量骤降
  if (minuteOfDay > 20 * 60) {  // 晚上 20:00 之后统计
    if (stats.totalTriggersToday < (avgTriggersHistory5Days / 2)) {
      updateRisk(RiskLevel::LIGHT);
      Serial.println("[ALERT] 今日活动量低于历史平均 50%");
    }
  }

  // 规则 4：长时间无活动（非夜间）
  bool normalSleepTime = (minuteOfDay >= 23 * 60 || minuteOfDay <= 6 * 60);
  if (!normalSleepTime && (now - stats.lastActivityMs > NO_ACTIVITY_WARN_MS) && !stats.noActivityWarnSent) {
    updateRisk(RiskLevel::MEDIUM);
    stats.noActivityWarnSent = true;
    beep(180, 2);
    Serial.println("[ALERT] 连续 2 小时无明显活动");
  }

  // 补充：白天长时间卧床
  bool dayTime = (minuteOfDay >= 8 * 60 && minuteOfDay <= 21 * 60);
  if (dayTime && stats.bedOccupied && stats.bedOccupiedStartMs > 0) {
    if (now - stats.bedOccupiedStartMs > BED_WARN_MS && !stats.bedWarnSent) {
      updateRisk(RiskLevel::MEDIUM);
      stats.bedWarnSent = true;
      beep(200, 3);
      sendRemoteAlert("长时间卧床", "白天连续 3 小时检测到床上有人", RiskLevel::MEDIUM);
      Serial.println("[ALERT] 白天长时间卧床，已预警");
    }
  }
}

void serialDashboard() {
  uint32_t now = millis();
  if (now - lastReportMs < 5000) {
    return;
  }
  lastReportMs = now;

  Serial.println("================ 状态看板 ================");
  Serial.printf("老人状态：%s\n", riskToString(stats.currentRisk));
  Serial.printf("今日起床：%s\n", stats.wakeupWarnSent ? "偏晚" : "正常");
  Serial.printf("卧床状态：%s\n", stats.bedOccupied ? "有人在床" : "离床");
  Serial.printf("厕所状态：%s\n", (stats.toiletDoorClosed && stats.toiletPirState) ? "有人滞留中" : "正常");
  Serial.printf("今日活动量(触发数)：%u\n", stats.totalTriggersToday);
  Serial.println("==========================================");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR_BEDROOM, INPUT);
  pinMode(PIN_PIR_TOILET, INPUT);
  pinMode(PIN_DOOR_TOILET, INPUT_PULLUP);
  pinMode(PIN_BED_PRESSURE, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  stats.lastActivityMs = millis();

  connectWiFi();
  connectMqtt();

  Serial.println("[INFO] 系统启动完成，开始监测...");
}

void loop() {
  stats.currentRisk = RiskLevel::NORMAL;  // 每轮重新评估当前风险

  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  scanSensors();
  evaluateRules();
  serialDashboard();

  delay(100);
}
