# Privacy Alert for Seniors Living Alone

基于 `ESP32 + 低成本非视觉传感器` 的独居老人生活异常预警原型（PlatformIO 工程）。

## 已实现功能（对应对话需求）

1. 起床异常检测  
规则：超过“历史平均起床时间 + 90 分钟”且仍在床上、卧室活动很少。

2. 厕所滞留分级预警  
规则：门关闭且厕所有人时开始计时。  
- 超过 12 分钟：本地蜂鸣提醒  
- 超过 18 分钟：升级为高风险，并通过 MQTT 远程告警

3. 白天长时间卧床检测  
规则：白天连续卧床超过 3 小时触发预警。

4. 长时间无活动检测  
规则：非睡眠时段连续 2 小时无明显活动触发预警。

5. 活动量骤降检测  
规则：20:00 后，当日活动触发次数低于最近 5 天平均值的 50%。

6. 三级告警机制  
- 正常（信息级）  
- 中度异常（警告级）  
- 高风险（严重级）

7. 展示与远程输出  
- 串口状态看板（每 5 秒输出）  
- Web 页面实时状态看板（`/`）  
- JSON 接口（`/api/status`）  
- MQTT 告警上报

## 默认硬件引脚

- 卧室 PIR：`GPIO14`
- 厕所 PIR：`GPIO27`
- 厕所门磁：`GPIO26`
- 床压传感器：`GPIO25`
- 蜂鸣器：`GPIO33`

## 配置项

请在 [`src/main.cpp`](/D:/competition/Privacy-Alert-for-Seniors-Living-Alone/Privacy-Alert-for-Seniors-Living-Alone/src/main.cpp) 顶部修改：

- `WIFI_SSID`
- `WIFI_PASS`
- `MQTT_BROKER_ADDR`
- `MQTT_BROKER_PORT`
- `MQTT_CLIENT_ID`
- `MQTT_USERNAME`
- `MQTT_PRIVATE_KEY`
- `MQTT_ALERT_TOPIC`

## 运行方式

```bash
platformio run
platformio run -t upload
platformio device monitor
```

设备连网后，在串口日志中查看 IP，然后浏览器访问：

- `http://<esp32-ip>/`
- `http://<esp32-ip>/api/status`

## 注意

当前“时间”采用 `millis()` 模拟日内分钟（上电即新的一天）。  
如果后续要用于长期实测，建议接入 RTC 或 NTP 进行真实时钟校准。
