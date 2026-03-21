# Privacy Alert for Seniors Living Alone

基于 `ESP32 + 非视觉传感器` 的独居老人生活异常预警原型（PlatformIO 工程）。

## 仓库结构

- `src/`：ESP32 固件
- `shared_protocol/`：共享协议与纯逻辑规则契约
- `docs/protocol.md`：字段、风险等级、原因码约定
- `tests/`：不依赖硬件的纯逻辑测试
- `windows_client/`：Windows MQTT 监控端

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

- 卧室 PIR：`GPIO34`
- 厕所 PIR：`GPIO35`
- 厕所门磁：`GPIO32`
- 床压传感器：`GPIO33`
- 蜂鸣器：`GPIO27`

## 网络与 MQTT

默认 MQTT 运行参数：
- MQTT 主机：`bemfa.com`
- MQTT 端口：`9501`
- MQTT Topic：`senioralertevents`

配置已分层：
- 编译期固定配置：`src/config_pins.h`、`src/config_rules.h`、`src/config_defaults.h`
- 运行时配置：SPIFFS 中的 `/config.json`
- 本地敏感配置：`src/secrets.h`

首次使用时：
1. 复制 `src/secrets.h.example` 为 `src/secrets.h`
2. 在 `src/secrets.h` 中填写 MQTT 私钥或 App 凭据
3. Wi-Fi 通过 AP 配网页或串口 `setwifi` 写入设备本地配置

协议字段和原因码见 [docs/protocol.md](/D:/Desktop/Zhao_zzZ/Privacy-Alert-for-Seniors-Living-Alone/docs/protocol.md)。

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

## 测试

```bash
python -m pip install -r requirements-dev.txt
pytest -q
```

当前测试覆盖：
- 起床异常规则
- 厕所滞留分级规则
- 白天卧床规则
- 无活动规则
- 活动量骤降规则
- 多规则并发时的风险汇总优先级

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

