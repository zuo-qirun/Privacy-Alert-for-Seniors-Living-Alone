import json
import queue
import threading
from dataclasses import dataclass
from datetime import datetime
from typing import Any

import tkinter as tk
from tkinter import messagebox, ttk

import paho.mqtt.client as mqtt


DEFAULT_BROKER = "bemfa.com"
DEFAULT_PORT = 9501
DEFAULT_TOPIC = "senioralertevents"
DEFAULT_PRIVATE_KEY = "84810b9b5f5245fdbc1e1738837f27a9"


@dataclass
class MqttConfig:
    broker: str
    port: int
    topic: str
    private_key: str
    app_id: str
    secret_key: str


class SeniorAlertClient:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("独居老人预警 MQTT 监控端")
        self.root.geometry("1120x760")
        self.root.minsize(1000, 700)

        self.event_queue: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.mqtt_client: mqtt.Client | None = None
        self.worker_lock = threading.Lock()

        self.connection_state = tk.StringVar(value="未连接")
        self.last_message_time = tk.StringVar(value="-")
        self.last_message_type = tk.StringVar(value="-")
        self.risk_text = tk.StringVar(value="normal")
        self.risk_cn_text = tk.StringVar(value="正常")
        self.status_text = tk.StringVar(value="-")
        self.device_time_text = tk.StringVar(value="-")
        self.topic_text = tk.StringVar(value=DEFAULT_TOPIC)

        self.sensor_vars = {
            "bedroom_pir": tk.StringVar(value="-"),
            "toilet_pir": tk.StringVar(value="-"),
            "toilet_door_closed": tk.StringVar(value="-"),
            "bed_occupied": tk.StringVar(value="-"),
            "wifi_connected": tk.StringVar(value="-"),
            "ap_mode": tk.StringVar(value="-"),
            "today_total_triggers": tk.StringVar(value="-"),
            "minute_of_day": tk.StringVar(value="-"),
        }

        self.alert_title_var = tk.StringVar(value="-")
        self.alert_detail_var = tk.StringVar(value="-")

        self.broker_var = tk.StringVar(value=DEFAULT_BROKER)
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))
        self.topic_var = tk.StringVar(value=DEFAULT_TOPIC)
        self.private_key_var = tk.StringVar(value=DEFAULT_PRIVATE_KEY)
        self.app_id_var = tk.StringVar(value="")
        self.secret_key_var = tk.StringVar(value="")

        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(150, self.process_events)

    def _build_ui(self) -> None:
        self.root.configure(bg="#edf2f7")

        main = ttk.Frame(self.root, padding=14)
        main.pack(fill=tk.BOTH, expand=True)

        top = ttk.Frame(main)
        top.pack(fill=tk.X, pady=(0, 10))

        self._build_connection_panel(top)
        self._build_overview_panel(top)

        middle = ttk.Frame(main)
        middle.pack(fill=tk.BOTH, expand=True)

        self._build_sensor_panel(middle)
        self._build_alert_panel(middle)

        bottom = ttk.LabelFrame(main, text="消息日志", padding=10)
        bottom.pack(fill=tk.BOTH, expand=True, pady=(10, 0))

        self.log_box = tk.Text(bottom, height=14, wrap=tk.WORD, bg="#111827", fg="#e5e7eb")
        self.log_box.pack(fill=tk.BOTH, expand=True)
        self.log_box.configure(state=tk.DISABLED)

    def _build_connection_panel(self, parent: ttk.Frame) -> None:
        panel = ttk.LabelFrame(parent, text="MQTT 连接", padding=10)
        panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 8))

        fields = [
            ("服务器", self.broker_var),
            ("端口", self.port_var),
            ("主题", self.topic_var),
            ("私钥", self.private_key_var),
            ("应用 ID", self.app_id_var),
            ("密钥", self.secret_key_var),
        ]

        for row, (label, var) in enumerate(fields):
            ttk.Label(panel, text=label, width=12).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
            show = "*" if label in ("私钥", "密钥") else None
            entry = ttk.Entry(panel, textvariable=var, width=34, show=show)
            entry.grid(row=row, column=1, sticky="ew", pady=4)

        panel.columnconfigure(1, weight=1)

        button_row = ttk.Frame(panel)
        button_row.grid(row=len(fields), column=0, columnspan=2, sticky="ew", pady=(10, 0))

        ttk.Button(button_row, text="连接", command=self.connect).pack(side=tk.LEFT)
        ttk.Button(button_row, text="断开", command=self.disconnect).pack(side=tk.LEFT, padx=8)
        ttk.Button(button_row, text="清空日志", command=self.clear_log).pack(side=tk.LEFT)

    def _build_overview_panel(self, parent: ttk.Frame) -> None:
        panel = ttk.LabelFrame(parent, text="概览", padding=10)
        panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        overview = [
            ("连接状态", self.connection_state),
            ("主题", self.topic_text),
            ("最近消息时间", self.last_message_time),
            ("最近消息类型", self.last_message_type),
            ("风险代码", self.risk_text),
            ("风险等级", self.risk_cn_text),
            ("状态", self.status_text),
            ("设备时间", self.device_time_text),
        ]

        for row, (label, var) in enumerate(overview):
            ttk.Label(panel, text=label, width=18).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
            ttk.Label(panel, textvariable=var).grid(row=row, column=1, sticky="w", pady=4)

        panel.columnconfigure(1, weight=1)

    def _build_sensor_panel(self, parent: ttk.Frame) -> None:
        panel = ttk.LabelFrame(parent, text="实时状态", padding=10)
        panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 8))

        rows = [
            ("卧室 PIR", "bedroom_pir"),
            ("厕所 PIR", "toilet_pir"),
            ("厕所门磁", "toilet_door_closed"),
            ("在床状态", "bed_occupied"),
            ("Wi-Fi", "wifi_connected"),
            ("AP 模式", "ap_mode"),
            ("今日触发数", "today_total_triggers"),
            ("当日分钟数", "minute_of_day"),
        ]

        for row, (label, key) in enumerate(rows):
            ttk.Label(panel, text=label, width=18).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
            ttk.Label(panel, textvariable=self.sensor_vars[key]).grid(row=row, column=1, sticky="w", pady=4)

    def _build_alert_panel(self, parent: ttk.Frame) -> None:
        panel = ttk.LabelFrame(parent, text="最近告警", padding=10)
        panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        ttk.Label(panel, text="标题", width=12).grid(row=0, column=0, sticky="nw", padx=(0, 8), pady=4)
        ttk.Label(panel, textvariable=self.alert_title_var, wraplength=320).grid(row=0, column=1, sticky="w", pady=4)

        ttk.Label(panel, text="详情", width=12).grid(row=1, column=0, sticky="nw", padx=(0, 8), pady=4)
        ttk.Label(panel, textvariable=self.alert_detail_var, wraplength=320).grid(row=1, column=1, sticky="w", pady=4)

        panel.columnconfigure(1, weight=1)

    def connect(self) -> None:
        with self.worker_lock:
            self.disconnect()

            try:
                config = self._read_config()
            except ValueError as exc:
                messagebox.showerror("配置错误", str(exc))
                return

            self.topic_text.set(config.topic)
            try:
                if config.private_key:
                    client_id = config.private_key
                    username = None
                    password = None
                elif config.app_id and config.secret_key:
                    client_id = "senior-alert-windows-client"
                    username = config.app_id
                    password = config.secret_key
                else:
                    raise ValueError("请填写私钥，或填写应用 ID 和密钥。")
            except ValueError as exc:
                messagebox.showerror("配置错误", str(exc))
                return

            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
            client.on_connect = self.on_connect
            client.on_disconnect = self.on_disconnect
            client.on_message = self.on_message

            if username:
                client.username_pw_set(username, password)

            self.mqtt_client = client
            self.connection_state.set("连接中...")
            self.append_log("正在连接 MQTT 服务器...")

            try:
                client.reconnect_delay_set(min_delay=1, max_delay=10)
                client.connect_async(config.broker, config.port, 60)
                client.loop_start()
            except Exception as exc:
                self.connection_state.set("连接失败")
                self.append_log(f"连接失败: {exc}")
                self.mqtt_client = None

    def disconnect(self) -> None:
        client = self.mqtt_client
        self.mqtt_client = None
        if client is None:
            return
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass
        self.connection_state.set("未连接")

    def _read_config(self) -> MqttConfig:
        broker = self.broker_var.get().strip()
        topic = self.topic_var.get().strip()
        private_key = self.private_key_var.get().strip()
        app_id = self.app_id_var.get().strip()
        secret_key = self.secret_key_var.get().strip()

        if not broker:
            raise ValueError("服务器地址不能为空。")
        if not topic:
            raise ValueError("主题不能为空。")

        try:
            port = int(self.port_var.get().strip())
        except ValueError as exc:
            raise ValueError("端口必须是数字。") from exc

        return MqttConfig(
            broker=broker,
            port=port,
            topic=topic,
            private_key=private_key,
            app_id=app_id,
            secret_key=secret_key,
        )

    def on_connect(self, client: mqtt.Client, _userdata: Any, flags: Any, reason_code: Any, _properties: Any) -> None:
        code_value = getattr(reason_code, "value", reason_code)
        if code_value == 0:
            topic = self.topic_var.get().strip()
            client.subscribe(topic)
            self.event_queue.put(("connected", topic))
        else:
            self.event_queue.put(("connect_error", f"原因={reason_code}, flags={flags}"))

    def on_disconnect(self, _client: mqtt.Client, _userdata: Any, reason_code: Any, _properties: Any) -> None:
        code_value = getattr(reason_code, "value", reason_code)
        self.event_queue.put(("disconnected", f"原因={code_value}"))

    def on_message(self, _client: mqtt.Client, _userdata: Any, msg: mqtt.MQTTMessage) -> None:
        try:
            payload = msg.payload.decode("utf-8")
            data = json.loads(payload)
        except Exception as exc:
            self.event_queue.put(("log", f"Invalid message: {exc}"))
            return
        self.event_queue.put(("message", {"topic": msg.topic, "data": data, "raw": payload}))

    def process_events(self) -> None:
        while True:
            try:
                event, value = self.event_queue.get_nowait()
            except queue.Empty:
                break

            if event == "connected":
                self.connection_state.set("已连接")
                self.append_log(f"已连接并订阅主题: {value}")
            elif event == "connect_error":
                self.connection_state.set("连接失败")
                self.append_log(f"连接错误: {value}")
            elif event == "disconnected":
                self.connection_state.set("未连接")
                self.append_log(f"连接断开: {value}")
            elif event == "log":
                self.append_log(str(value))
            elif event == "message":
                self.handle_message(value["topic"], value["data"], value["raw"])

        self.root.after(150, self.process_events)

    def handle_message(self, topic: str, data: dict[str, Any], raw: str) -> None:
        self.last_message_time.set(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        self.last_message_type.set(str(data.get("type", "-")))
        self.topic_text.set(topic)

        self.risk_text.set(str(data.get("risk", self.risk_text.get())))
        self.risk_cn_text.set(str(data.get("risk_cn", self.risk_cn_text.get())))
        self.status_text.set(str(data.get("status_cn", self.status_text.get())))
        self.device_time_text.set(str(data.get("time", data.get("now_time", self.device_time_text.get()))))

        self._update_sensor_var("bedroom_pir", data.get("bedroom_pir"))
        self._update_sensor_var("toilet_pir", data.get("toilet_pir"))
        self._update_sensor_var("toilet_door_closed", data.get("toilet_door_closed"), true_text="关闭", false_text="打开")
        self._update_sensor_var("bed_occupied", data.get("bed_occupied"), true_text="有人", false_text="无人")
        self._update_sensor_var("wifi_connected", data.get("wifi_connected"), true_text="已连接", false_text="未连接")
        self._update_sensor_var("ap_mode", data.get("ap_mode"), true_text="开启", false_text="关闭")

        if "today_total_triggers" in data:
            self.sensor_vars["today_total_triggers"].set(str(data["today_total_triggers"]))
        if "minute_of_day" in data:
            self.sensor_vars["minute_of_day"].set(str(data["minute_of_day"]))

        if data.get("type") == "alert":
            self.alert_title_var.set(str(data.get("title", "-")))
            self.alert_detail_var.set(str(data.get("detail", "-")))

        self.append_log(raw)

    def _update_sensor_var(self, key: str, value: Any, true_text: str = "触发", false_text: str = "空闲") -> None:
        if value is None:
            return
        if isinstance(value, bool):
            self.sensor_vars[key].set(true_text if value else false_text)
            return
        self.sensor_vars[key].set(str(value))

    def append_log(self, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.insert(tk.END, f"[{stamp}] {message}\n")
        self.log_box.see(tk.END)
        self.log_box.configure(state=tk.DISABLED)

    def clear_log(self) -> None:
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.delete("1.0", tk.END)
        self.log_box.configure(state=tk.DISABLED)

    def on_close(self) -> None:
        self.disconnect()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    app = SeniorAlertClient(root)
    app.append_log("客户端已就绪。")
    root.mainloop()


if __name__ == "__main__":
    main()
