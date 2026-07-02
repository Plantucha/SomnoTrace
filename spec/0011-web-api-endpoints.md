# 0011 — Web API Endpoints Contract

- **Status:** Proposed
- **Author(s):** Antigravity
- **Created:** 2026-07-02
- **Last updated:** 2026-07-02
- **Related specs:** `0008-config-and-network-lifecycle.md`, `0009-web-interface.md`, `0010-web-ui-architecture-and-design.md`

## 1. Summary

This specification defines the JSON and binary data communication protocol between the SomnoTrace client web application and the ESP32-S3 web server. It outlines standard endpoints for real-time status telemetry, Wi-Fi environment scanning, persistent configurations, compressed binary therapy data streaming, and real-time console log streaming.

## 2. Motivation / goals

- **High Bandwidth Efficiency:** ESP32-S3 web servers are network-constrained. JSON payload size must be minimized, and high-frequency timeseries data must be sent as compact binary streams to avoid memory exhaustion and packet fragmentation.
- **Security by Default:** Secrets (passwords, API tokens) must never be transmitted from the device to the browser under any circumstance.
- **Real-Time Control:** Enable smooth telemetry graphs (CPU, heap) and log streaming without polling, lowering CPU load on the ESP32-S3.

## 3. Non-goals

- General-purpose databases or file managers.
- Providing cloud synchronization protocols directly through these endpoints (cloud upload is handled in the background by the upload component).

---

## 4. Endpoints & Data Contracts

### 4.1 System Status (`GET /api/status`)
Used to populate the **Status Tab** and dashboard indicators.

#### JSON Response Format:
```json
{
  "uptime_s": 86420,
  "timezone": "GMT+10:00",
  "ip_address": "192.168.1.120",
  "ntp_sync": true,
  "cpu_load_pct": 12,
  "memory": {
    "internal_free_bytes": 102400,
    "internal_min_free_bytes": 81920,
    "psram_free_bytes": 6291456,
    "psram_min_free_bytes": 5242880
  },
  "wifi": {
    "connected_ssid": "Home-Network",
    "rssi": -65,
    "configured_ssids": ["Home-Network", "Work-Network", "Mobile-Hotspot"]
  },
  "sd_card": {
    "mounted": true,
    "total_kb": 15500000,
    "free_kb": 12200000
  },
  "therapy_active": false,
  "ble": {
    "as11_paired": true,
    "as11_device_name": "AirSense 11 #12345",
    "o2ring_paired": false,
    "o2ring_device_name": ""
  }
}
```

---

### 4.2 Wi-Fi Scanning (`GET /api/scan`)
Triggers or returns cached Wi-Fi environment data. BSSID de-duplication is performed by the client application using RSSI signals.

#### JSON Response Format:
```json
{
  "networks": [
    { "ssid": "Home-Network", "rssi": -45, "secure": true },
    { "ssid": "Home-Network", "rssi": -62, "secure": true },
    { "ssid": "Neighbor-Guest", "rssi": -78, "secure": false },
    { "ssid": "Work-Network", "rssi": -55, "secure": true }
  ]
}
```

---

### 4.3 Device Settings Config (`GET /api/config` & `POST /api/config`)

#### A. Fetching Config (`GET /api/config`)
Returns the currently stored parameters. **All secret fields must be masked if populated.**

##### JSON Response Format:
```json
{
  "hostname": "SomnoTrace",
  "timezone": "GMT+10:00",
  "wifi_profiles": [
    { "ssid": "Home-Network", "password_set": true },
    { "ssid": "Work-Network", "password_set": true },
    { "ssid": "Mobile-Hotspot", "password_set": false },
    { "ssid": "", "password_set": false }
  ],
  "smb": {
    "host": "192.168.1.100",
    "share": "sleep",
    "user": "guest",
    "path": "/SomnoTrace",
    "password_set": false
  },
  "sleephq": {
    "client_id": "shq_cl_abc123",
    "client_secret_set": true
  }
}
```

#### B. Saving Config (`POST /api/config`)
Updates NVS settings.
- If a password field contains the mask value `"***"`, the web server **must ignore it** and retain the existing value in NVS.
- To clear a password, an empty string `""` is sent.

##### JSON Request Format:
```json
{
  "hostname": "SomnoTrace-New",
  "timezone": "GMT+10:00",
  "wifi_profiles": [
    { "ssid": "Home-Network", "password": "***" },
    { "ssid": "New-Network", "password": "securepassword123" },
    { "ssid": "", "password": "" },
    { "ssid": "", "password": "" }
  ],
  "smb": {
    "host": "192.168.1.100",
    "share": "sleep",
    "user": "guest",
    "path": "/SomnoTrace",
    "password": ""
  },
  "sleephq": {
    "client_id": "shq_cl_abc123",
    "client_secret": "***"
  }
}
```
##### JSON Response Format:
`200 OK`
```json
{ "status": "ok" }
```

---

### 4.4 Therapy Session Data (`GET /api/session/data`)
Serves the session listings or downsampled timeseries datasets.

#### A. Query Parameters:
- `date`: Date string in `YYYY-MM-DD` format (representing the noon-to-noon boundary).
- `downsample`: Number of data points to downsample the timeseries stack to (default: `1000`). If set to `0`, serves full-resolution binary streams.

#### B. JSON Meta-Data Response (Default when no stream format requested):
Returns metadata, statistics, and downsampled curves for the selected night. High-frequency curves (such as raw airflow) are downsampled to avoid memory crashes on the ESP32.

```json
{
  "session_id": "20231027_221430",
  "date": "2023-10-27",
  "start_time": "2023-10-27T22:14:30Z",
  "end_time": "2023-10-28T06:30:15Z",
  "summary": {
    "ahi": 1.2,
    "usage_ms": 29745000,
    "avg_pressure_cmh2o": 9.8,
    "leak_95_lmin": 4.2,
    "leak_70_lmin": 1.5
  },
  "timestamps": [0, 60, 120, 180],
  "series": {
    "airflow": [0.2, 0.4, -0.1, -0.3],
    "pressure": [4.0, 6.5, 9.8, 9.8],
    "leak": [0.0, 0.2, 1.2, 0.8],
    "spo2": [98, 97, 96, 96],
    "pulse": [62, 64, 68, 67]
  }
}
```

#### C. Optimized Binary Stream Mode (`GET /api/session/binary?date=YYYY-MM-DD&channel=name`)
For high-speed loading of full-resolution data, the browser can fetch channels as packed binary float arrays instead of parsing heavy JSON.
- **Response headers**: `Content-Type: application/octet-stream`
- **Body format**: A simple binary packet header followed by a sequence of 32-bit floats.

##### Binary Packet Layout:
```
Offset (Bytes) | Type       | Field
------------------------------------------------
0 - 7          | uint64_t   | Start timestamp (Epoch MS)
8 - 11         | float      | Sample rate (Hz)
12 - 15        | uint32_t   | Number of samples (N)
16+            | float[N]   | Packed sample data (IEEE 754 float)
```

---

### 4.5 Live Logs Stream (`GET /api/logs/stream`)
Real-time streaming console logs using **Server-Sent Events (SSE)**.
- **Response headers**:
  - `Content-Type: text/event-stream`
  - `Cache-Control: no-cache`
  - `Connection: keep-alive`

#### SSE Stream Format:
```
event: log
data: I (1250) somnotrace: Wi-Fi Connected, IP=192.168.1.120

event: log
data: I (1280) upload_smb: SMB connected
```

---

## 5. Security / privacy considerations

- Wi-Fi and cloud upload settings are saved in an **encrypted NVS partition** to prevent raw credential theft in case of device physical tampering.
- Accessing the configuration dashboard requires standard local network proximity.
- No health data or telemetry contains patient names or identifiable clinical metadata.

## 6. Acceptance criteria

- [ ] All GET and POST requests utilize validation on config inputs (e.g. matching standard GMT regex).
- [ ] Stored passwords in NVS are never serialized back to clients (use `"password_set": true`).
- [ ] Standardized binary stream mode is available to fetch full-resolution timeseries channels.
- [ ] Live log streaming does not allocate dynamic buffers inside the central print loops to protect heap memory.

## 7. Changelog

- 2026-07-02: Initial API endpoints contract specification.
