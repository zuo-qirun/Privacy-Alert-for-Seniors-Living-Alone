# Protocol Contract

This repository now keeps a shared protocol contract across firmware, dashboard, MQTT payloads, and the Windows client.

## Risk Levels

- `normal`
- `warning`
- `critical`

Firmware keeps backward compatibility fields `risk` and `risk_cn`, but new integrations should read:

- `risk_level`
- `reason_code`

## Reason Codes

- `none`
- `late_wakeup`
- `toilet_stay_12m`
- `toilet_stay_18m`
- `daytime_bedrest_3h`
- `inactive_2h`
- `activity_drop_50pct`

## `/api/status`

Core fields:

- `type`: always `status`
- `risk_level`: current aggregated risk level
- `reason_code`: current primary reason
- `status_cn`: Chinese label for the current aggregated risk
- `bedroom_pir`
- `toilet_pir`
- `toilet_door_closed`
- `bed_occupied`
- `today_total_triggers`
- `avg_total_triggers_5d`
- `avg_wakeup_minute_5d`
- `wifi_connected`
- `wifi_ssid`
- `wifi_ip`
- `ap_mode`
- `now_time`

Compatibility fields retained for existing consumers:

- `risk`
- `risk_cn`
- `wakeup_warn`
- `toilet_warn`
- `toilet_critical`
- `bed_warn`
- `no_activity_warn`
- `activity_drop_warn`

## MQTT Payloads

All MQTT payloads now carry:

- `type`
- `risk_level`
- `reason_code`
- `time`

Message types:

- `alert`: emitted when a rule first enters an alerting episode
- `status`: periodic heartbeat with aggregate risk state
- `sensor_realtime`: high-frequency sensor snapshot

`alert` also includes:

- `title`
- `detail`
- `today_total_triggers`

## Reference Implementations

- Firmware constants: `src/alert_protocol.h`
- Python protocol constants: `shared_protocol/protocol.py`
- Python pure rule contract: `shared_protocol/rule_logic.py`
