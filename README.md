# Privacy Alert for Seniors Living Alone

基于 `ESP32 + 非视觉传感器` 的独居老人生活异常预警原型（PlatformIO 工程）。

## 已实现功能

1. 起床异常检测  
规则：超过“历史平均起床时间 + 90 分钟”且仍在床上、卧室活动很少。

2. 厕所滞留分级预警  
规则：门关闭且检测到人体后开始计时。  
- 超过 12 分钟：本地蜂鸣提醒  
- 超过 18 分钟：升级为高风险，并通过 MQTT 远程告警

3. 白天长时间卧床检测  
规则：白天连续卧床超过 3 小时触发预警。

4. 长时间无活动检测  
规则：非睡眠时段连续 2 小时无明显活动触发预警。

5. 活动量骤降检测  
规则：20:00 后，当日活动触发次数低于近 5 天平均值的 50% 触发预警。

6. 三级风险状态  
- 正常
- 中度异常
- 高风险

7. 可视化与调试  
- 串口状态看板（每 5 秒输出）
- 串口调试命令（可读取指定引脚数字/模拟值）
- Web 页面状态看板：`/`
- JSON 接口：`/api/status`
- 离线时 AP 配网页面与接口：`/api/wifi-config`

## 默认引脚

- 卧室 PIR：`GPIO14`
- 厕所 PIR：`GPIO27`
- 厕所门磁：`GPIO26`
- 床压传感器：`GPIO25`
- 蜂鸣器：`GPIO33`

## 网络与 MQTT

当前代码中配置：
- MQTT 主机：`bemfa.com`
- MQTT 端口：`9501`
- MQTT 私钥：已在 `src/main.cpp` 中配置

Wi-Fi 相关配置位于 [`src/main.cpp`](/D:/competition/Privacy-Alert-for-Seniors-Living-Alone/Privacy-Alert-for-Seniors-Living-Alone/src/main.cpp)：
- `WIFI_SSID`
- `WIFI_PASS`
- `AP_SSID`
- `AP_PASS`

## 运行方式

```bash
platformio run
platformio run -t upload
platformio device monitor
```

设备联网后，在串口日志查看 IP，然后访问：
- `http://<esp32-ip>/`
- `http://<esp32-ip>/api/status`

如果连续 3 次 Wi-Fi 连接失败，设备会自动进入 AP 配网模式。  
手机连接 AP 后访问页面即可提交 Wi-Fi 账号密码，设备会退出 AP 并重连 STA。

## 串口命令

串口波特率：`115200`  
支持行尾：`LF / CRLF / CR`

- `help`：查看命令帮助
- `pins`：查看关键引脚数字值/模拟值
- `d <pin>`：读取指定 GPIO 数字电平
- `a <pin>`：读取指定 GPIO 模拟输入
- `wifi`：查看当前 Wi-Fi / AP 状态
- `setwifi <ssid> <pwd>`：更新 Wi-Fi 并立即重连
- `reconnect`：立即触发重连

