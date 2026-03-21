# Windows MQTT Client

本目录提供一个 Windows 桌面客户端，用于通过 MQTT 实时接收 ESP32 设备发出的状态数据并可视化显示。

## 功能

- 连接 `bemfa.com:9501`
- 订阅当前设备主题 `senioralertevents`
- 实时展示：
  - 风险等级
  - 原因码（`reason_code`）
  - 设备状态
  - 卧室 PIR / 厕所 PIR / 门磁 / 床压
  - Wi-Fi / AP 模式
  - 最近告警
  - 原始消息日志

## 安装

```bash
cd windows_client
python -m pip install -r requirements.txt
```

## 配置

默认不再把私钥硬编码在 `app.py` 中。

首次使用时：
1. 复制 `config.example.json` 为 `config.json`
2. 在 `config.json` 中填写 `private_key`，或填写 `app_id` 和 `secret_key`
3. 运行客户端，必要时在界面中继续调整 Broker / Port / Topic

## 运行

```bash
cd windows_client
python app.py
```

## 连接方式

默认配置：

- Broker: `bemfa.com`
- Port: `9501`
- Topic: `senioralertevents`

认证优先级：

1. 如果填写了 `private_key`，客户端会使用“私钥作为 clientId”连接
2. 如果没有私钥，但填写了 `app_id` 和 `secret_key`，则使用账号密码认证

## 协议

客户端优先读取共享协议字段：

- `risk_level`
- `reason_code`
- `risk_level_cn`
- `status_cn`

共享契约见根目录的 `shared_protocol/` 和 `docs/protocol.md`。
