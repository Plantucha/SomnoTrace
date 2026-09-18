# 0013 — MQTT & Home Assistant Integration

- **Status:** Proposed
- **Author(s):** Ilya Kruchinin, Antigravity
- **Created:** 2026-09-18
- **Last updated:** 2026-09-18
- **Related specs:** `0008-config-and-network-lifecycle.md`, `0011-web-api-endpoints.md`

---

## 1. Summary

This specification defines the MQTT communication protocol, entity model, and Home Assistant Auto-Discovery integration for SomnoTrace. It provides real-time state and event telemetry (therapy start/stop, mask-off safety alarms, Bluetooth connection status, battery levels, and optional remote switches) over a single persistent TCP connection. Every entity is sourced directly from existing in-memory variables with zero on-chip statistical calculations.

---

## 2. Motivation / Goals

- **First-Class Smart Home Integration:** Enable instant bedtime routines (e.g. lights off, blinds down, sleep HVAC mode when therapy starts), morning wake-up routines (coffee maker on, warm lights when therapy stops), and emergency alerts (smartwatch vibration if mask falls off at 3 AM).
- **Superior Reliability over WiFi SD Cards:** Existing community solutions (such as `hms-cpap`) rely on fragile, hot-running WiFi SD cards (e.g. ezShare) that frequently drop Wi-Fi or risk FAT filesystem corruption. SomnoTrace with ESP32-S3 BLE is fundamentally more robust.
- **Eliminate HTTP Socket Exhaustion:** Polling `/api/status` every 5–10 seconds over HTTP consumes one of the 10 limited lwIP sockets on the ESP32. MQTT maintains a single persistent connection with sub-second event latency and near-zero CPU/bandwidth overhead.
- **Zero-Configuration Setup:** Support Home Assistant MQTT Auto-Discovery so that entering an MQTT broker address automatically provisions all entities with sensible device names, icons, and categories.

---

## 3. Non-Goals & Architectural Decision Records

This section explicitly documents what was excluded from the MQTT design and the technical rationale behind each decision:

### 3.1 No On-Chip Clinical Calculations (AHI, 95th Percentiles, Compliance)
* **Rationale:** In SomnoTrace, all heavy statistical calculations are intentionally offloaded to the client-side JavaScript engine in `portal.html` or to clinical viewers (OSCAR, SleepHQ). Computing 95th percentile mask pressure or leak requires sorting arrays of hundreds of thousands of samples, and AHI requires dividing annotated event counts by filtered usage hours.
* **Smart Home Mismatch:** Home automations do not trigger on 95th percentile values or historical AHI.
* **Native Alternative:** Home Assistant already contains a built-in `history_stats` platform that calculates daily compliance hours and session totals natively from the binary `in_therapy` sensor in 4 lines of YAML.

### 3.2 No Live Vitals Streaming ($\text{SpO}_2$, Pulse, Mask Pressure)
* **O2 Ring Hardware Architecture:** Wellue O2 Rings (both Gen 1 and Gen 2) record internally during sleep and only transfer data as a batch file **after** the ring is removed from the finger in the morning. Gen 1 turns off BLE advertising while on-finger; Gen 2 (OxyII) connects every 30–60 seconds strictly to run an off-finger check (`LIVE_B`), because downloading data while on-finger is forbidden by ring firmware. A 30–60s check is too infrequent to serve as "live" vitals.
* **Nonin Inconsistency:** While Nonin WristOx2 3150 does stream 1 Hz vitals via the AS11, very few users own this expensive adapter. Having sensors that work for Nonin but remain permanently unavailable for O2 Ring creates an inconsistent user experience.
* **Oscillating Waveforms:** Mask pressure oscillates at 25 Hz with every breath. Sampling every 5–10s simply captures random points on a breathing curve (e.g. 5.1 $\text{cmH}_2\text{O}$ on exhale vs 9.2 on inhale), providing zero smart-home utility.

### 3.3 No 25 Hz Waveforms or Log Streaming
* **Rationale:** High-frequency flow and pressure waveforms remain strictly in `.snt` and `.edf` files on the SD card. Console logs belong in Syslog (RFC 5424) or serial output. Streaming them over MQTT would flood Wi-Fi airtime and broker queues.

---

## 4. Behaviour & Data Contracts

### 4.1 Connection Lifecycle & Last Will and Testament (LWT)
- **Client Library:** Built-in ESP-IDF `esp-mqtt` (`components/mqtt`).
- **Single Persistent TCP Stream:** Maintains a single long-lived TCP connection (port 1883 or TLS 8883). Publishing telemetry does not open new sockets.
- **Base Topic:** `somnotrace/<client_id>/` (default: `somnotrace/`).
- **LWT (Availability):**
  - Topic: `<base_topic>/status`
  - Online Payload: `online` (retained, published upon connect)
  - Offline Payload: `offline` (retained, registered as LWT)
  - When SomnoTrace powers off or reboots, Home Assistant marks all entities as `unavailable`.

### 4.2 Publishing Model: Combined JSON State Topic & Single Timer

To minimize network packets, CPU wakeups, and broker overhead, SomnoTrace avoids publishing to separate discrete topics. Instead, it adopts the standard **Combined JSON State Topic** pattern:

- **State Topic:** `<base_topic>/state`
- **Payload Example:**
  ```json
  {
    "in_therapy": true,
    "alert_state": "armed",
    "as11_connected": true,
    "oximeter_connected": false,
    "battery_percent": 85,
    "battery_charging": false,
    "wifi_rssi": -65,
    "therapy_duration_min": 142,
    "uptime_s": 86400
  }
  ```
- **Cadence & Triggers (Single-Timer + Event-Driven Hybrid):**
  1. **Fixed 60-Second Telemetry Timer (Non-Configurable):** A single timer fires every 60 seconds to sample in-memory battery, Wi-Fi RSSI, duration, and uptime, publishing the combined JSON state. The 60-second interval is intentionally hardcoded and non-configurable to protect 2.4 GHz radio coexistence and prevent Wi-Fi TX/retransmissions from contending with the active 25 Hz BLE StreamData reception during sleep.
  2. **Immediate Event Bypass:** Any state transition (therapy start/stop, alert state change, charger connect/disconnect) bypasses the timer and immediately publishes an updated state JSON (< 50 ms latency).

### 4.3 Home Assistant MQTT Auto-Discovery
Upon connecting to the broker, SomnoTrace publishes retained discovery payloads to:
`homeassistant/<component>/somnotrace/<object_id>/config`

Each entity references `state_topic: "<base_topic>/state"` and extracts its field via `value_template: "{{ value_json.<key> }}"`. All entities update simultaneously in Home Assistant in a single transaction.

All entities are bound to a single Home Assistant Device:
```json
"device": {
  "identifiers": ["somnotrace_<mac_or_id>"],
  "name": "SomnoTrace",
  "model": "ESP32-S3-Touch-LCD-1.54",
  "manufacturer": "SomnoTrace",
  "sw_version": "v2.0.x"
}
```

### 4.4 Entity Catalog (State & Telemetry)

#### Tier 1: Core Automation Triggers (Immediate / Event-Driven)
*Published immediately when state changes occur.*

| Entity ID | Component | JSON Key | In-Memory Source | HA Entity | Automation Use Case |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `in_therapy` | `binary_sensor` | `in_therapy` | `session_writer_is_active()` | `binary_sensor.somnotrace_in_therapy` | **The #1 trigger:** Bedtime & wakeup scenes. |
| `alert_state` | `sensor` | `alert_state` | `therapy_alert_get_state()` | `sensor.somnotrace_alert_state` | **Mask-off alarm:** Trigger smartwatch or lights if therapy stops unexpectedly at night. |
| `as11_connected` | `binary_sensor` | `as11_connected` | `as11_ble_get_status() == "connected"` | `binary_sensor.somnotrace_cpap_connected` | Machine powered on & in Bluetooth range. |
| `oximeter_connected` | `binary_sensor` | `oximeter_connected` | `oximeter_get_status() == "connected"` | `binary_sensor.somnotrace_oximeter_connected` | O2 Ring / Nonin connected via BLE. |

#### Tier 2: Device & Power Health (Periodic 60s Telemetry)
*Sampled and published by the single 60s telemetry timer.*

| Entity ID | Component | JSON Key | In-Memory Source | Unit | Description |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `battery_percent` | `sensor` | `battery_percent` | `bsp_power_battery_get(&batt)` | `%` | Battery level (device class: `battery`). |
| `battery_charging` | `binary_sensor` | `battery_charging` | `batt.charging` | `ON`/`OFF` | USB charging state (device class: `battery_charging`). |
| `wifi_rssi` | `sensor` | `wifi_rssi` | `netprov_get_link(&link)` | `dBm` | Wi-Fi signal strength (device class: `signal_strength`). |
| `therapy_duration_min` | `sensor` | `therapy_duration_min` | `(now_ms - s->start_epoch_ms) / 60000` | `min` | Elapsed duration of current active session. |
| `uptime_s` | `sensor` | `uptime_s` | `xTaskGetTickCount()` | `s` | System uptime (device class: `duration`). |

#### Tier 3: Optional Diagnostic Telemetry (Included in 60s Payload)
*Available in RAM, provided as optional diagnostic sensors.*

| Entity ID | Component | JSON Key | In-Memory Source | Description / Caveats |
| :--- | :--- | :--- | :--- | :--- |
| `upload_status` | `sensor` | `upload_status` | `uploader_get_summary(&pending, &worst)` | Current worst uploader state (`"idle"`, `"uploading"`, `"cooldown"`). Note: `"cooldown"` is a transient retry state, not a permanent failure. |
| `upload_pending` | `sensor` | `upload_pending` | `uploader_get_summary()` | Total units (days/files) remaining across all configured backends. |
| `sd_free_mb` | `sensor` | `sd_free_mb` | `s_status_cache.sd_free / (1024*1024)` | Free space on SD card in megabytes. |

---

### 4.4 Bidirectional Command Topics (Optional Controls)

Home Assistant can control the CPAP or acknowledge alarms without opening the SomnoTrace web interface:

| Command Topic | Expected Payload | Firmware Action |
| :--- | :--- | :--- |
| `<base_topic>/cmd/therapy` | `START` or `STOP` | Invokes `EnterTherapy` or `EnterStandby` via the existing AS11 BLE JSON-RPC bridge. Creates a `switch.somnotrace_therapy` in Home Assistant. |
| `<base_topic>/cmd/alert` | `ACK` or `SILENCE` | Invokes `therapy_alert_acknowledge()` to silence buzzer when mask-off is intentional. |
| `<base_topic>/cmd/uploader` | `SCAN` or `RETRY` | Invokes `upload_sched_request_scan()` to immediately retry pending uploads. |

---

### 4.5 Configuration & NVS Schema

Configuration is stored in NVS under the `netprov` / `mqtt` namespace:
- `mqtt_en`: `uint8_t` (0 = disabled, 1 = enabled)
- `mqtt_uri`: string (e.g. `mqtt://192.168.1.50:1883` or `mqtts://...`)
- `mqtt_user`: string (optional username)
- `mqtt_pass`: string (optional password)
- `mqtt_prefix`: string (default: `somnotrace`)
- `mqtt_discovery`: `uint8_t` (default: 1 = enabled)

---

## 5. Home Assistant Native Duration Tracking

To satisfy compliance tracking without any on-chip calculation overhead, users configure Home Assistant's native `history_stats` platform:

```yaml
sensor:
  - platform: history_stats
    name: "CPAP Sleep Today"
    entity_id: binary_sensor.somnotrace_in_therapy
    state: "on"
    type: time
    start: "{{ now().replace(hour=12, minute=0, second=0) - timedelta(days=1) }}"
    end: "{{ now() }}"
```

---

## 6. Acceptance Criteria

- [ ] MQTT subsystem initializes without impacting real-time BLE reception or SD card recording.
- [ ] Connects to unencrypted (1883) and TLS (8883) MQTT brokers with or without credentials.
- [ ] Publishes Last Will and Testament (`<prefix>/status` = `offline`) on unexpected disconnection.
- [ ] Publishes Home Assistant MQTT discovery payloads upon initial connection.
- [ ] Publishes `in_therapy` transition (`ON`/`OFF`) within 1.0 second of therapy start/stop.
- [ ] Publishes `alert_state` transitions immediately when `therapy_alert` state changes.
- [ ] Responds to `<prefix>/cmd/therapy` commands (`START`/`STOP`) by calling the RPC bridge.
- [ ] Silences therapy alert buzzer when `<prefix>/cmd/alert` receives `ACK`.
- [ ] No allocations from internal SRAM (`DIRAM`); client stack and buffers allocate from PSRAM.

---

## 7. Security / Privacy Considerations

- **Confidentiality on LAN:** SomnoTrace transmits medical state (therapy active, alert status). Users should configure an authenticated broker on a private local network or use TLS (`mqtts://`).
- **No Patient Data:** No patient names, serial numbers, clinical notes, or raw physiological waveforms are published over MQTT.
- **NVS Protection:** MQTT broker passwords stored in NVS are never displayed or returned in `/api/status`.

---

## 8. Changelog

- 2026-09-18: Initial specification drafted. Documented non-goals (exclusion of on-chip clinical math, 25 Hz waveforms, and live vitals due to O2 Ring batching architecture). Defined Tier 1–3 entity catalog and bidirectional command topics.
