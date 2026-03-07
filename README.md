# Privacy-Alert-for-Seniors-Living-Alone

基于 **ESP32 + 多传感器** 的独居老人隐私友好型告警原型（PlatformIO 工程）。

## 系统结构

- 卧室区域：卧室 PIR + 压力垫
- 厕所区域：厕所 PIR + 门磁
- 主控中心：ESP32 + 蜂鸣器 + Wi-Fi + MQTT 远程告警

## 目录结构

- `platformio.ini`：PlatformIO 编译配置
- `src/main.cpp`：单片机主程序（包含引脚定义、规则引擎、MQTT 告警机制）

## 引脚分配（默认）

> 可在 `src/main.cpp` 顶部常量处直接修改。

- 卧室 PIR：`GPIO14`
- 厕所 PIR：`GPIO27`
- 厕所门磁：`GPIO26`
- 床边压力垫：`GPIO25`
- 蜂鸣器：`GPIO33`

## 已实现核心功能

1. 起床异常检测（平均起床时间 + 容忍窗口）
2. 长时间卧床检测（白天持续 3 小时）
3. 厕所滞留分级检测（12 分钟本地提醒、18 分钟 MQTT 远程告警）
4. 活动量骤降检测（低于历史均值 50%）
5. 长时间无活动检测（非睡眠时段）

## 使用方式

```bash
# 安装 PlatformIO CLI 后执行
pio run
pio run -t upload
pio device monitor
```

## 配置项

请在 `src/main.cpp` 中修改：

- `WIFI_SSID`
- `WIFI_PASS`
- `MQTT_BROKER_ADDR`
- `MQTT_BROKER_PORT`
- `MQTT_CLIENT_ID`
- `MQTT_USERNAME`
- `MQTT_PRIVATE_KEY`（私钥）
- `MQTT_ALERT_TOPIC`

如需适配传感器高低电平逻辑，请调整 `scanSensors()` 中 `digitalRead(...) == HIGH` 的判断。
