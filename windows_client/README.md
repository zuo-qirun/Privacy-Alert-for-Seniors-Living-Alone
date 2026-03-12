# Windows MQTT Client

本目录提供一个 Windows 桌面客户端，用于通过 MQTT 实时接收 ESP32 设备发出的状态数据并可视化显示。

## 功能

- 连接 `bemfa.com:9501`
- 订阅当前设备主题 `senioralertevents`
- 实时展示：
  - 风险等级
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

## 运行

```bash
cd windows_client
python app.py
```

## 连接方式

默认配置已经填好：

- Broker: `bemfa.com`
- Port: `9501`
- Topic: `senioralertevents`

认证优先级：

1. 如果填写了 `Private Key`，客户端会使用“私钥作为 clientId”连接
2. 如果没有私钥，但填写了 `App ID` 和 `Secret Key`，则使用账号密码认证

## 备注

- 客户端目前是 Python + Tkinter 实现，不需要额外 UI 框架。
- 如果你后面要打包成 `.exe`，可以再用 `pyinstaller` 处理。
