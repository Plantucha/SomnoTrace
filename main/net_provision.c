/*
 * SomnoTrace - Wi-Fi provisioning, SoftAP captive portal, and NVS config
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include "net_provision.h"
#include "as11_ble.h"
#include "time_sync.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "netprov";

#define NVS_NAMESPACE       "cfg"
#define NVS_KEY_HOSTNAME    "hostname"
#define NVS_KEY_SSID_FMT    "ssid%d"
#define NVS_KEY_PASS_FMT    "pass%d"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_STA_RETRY       3

static EventGroupHandle_t s_wifi_events;
static int s_retry_num = 0;
static volatile bool s_connecting = false;
static volatile bool s_connected = false;
static char s_got_ip[16];
static httpd_handle_t s_httpd = NULL;
static bool s_portal_mode = false;
static char s_connected_ip[16] = "0.0.0.0";
static char s_ap_ssid[NETPROV_HOSTNAME_MAXLEN + 8];
static uint32_t s_ap_ip = 0;

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;

/* ------------------------------------------------------------------ */
/*  NVS config storage                                                */
/* ------------------------------------------------------------------ */
bool netprov_load_config(struct netprov_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->hostname, "SomnoTrace", sizeof(cfg->hostname));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = sizeof(cfg->hostname);
    nvs_get_str(h, NVS_KEY_HOSTNAME, cfg->hostname, &len);

    bool any = false;
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_SSID_FMT, i + 1);
        size_t ssid_len = sizeof(cfg->wifi[i].ssid);
        if (nvs_get_str(h, key, cfg->wifi[i].ssid, &ssid_len) == ESP_OK
            && cfg->wifi[i].ssid[0] != '\0') {
            any = true;
            snprintf(key, sizeof(key), NVS_KEY_PASS_FMT, i + 1);
            size_t pass_len = sizeof(cfg->wifi[i].pass);
            nvs_get_str(h, key, cfg->wifi[i].pass, &pass_len);
        }
    }
    nvs_close(h);
    return any;
}

esp_err_t netprov_save_config(const struct netprov_config *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, NVS_KEY_HOSTNAME, cfg->hostname);
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_SSID_FMT, i + 1);
        nvs_set_str(h, key, cfg->wifi[i].ssid);
        snprintf(key, sizeof(key), NVS_KEY_PASS_FMT, i + 1);
        nvs_set_str(h, key, cfg->wifi[i].pass);
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/*  WiFi events                                                       */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_connecting) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            ESP_LOGI(TAG, "Wi-Fi link lost, retrying connection indefinitely...");
            esp_wifi_connect();
        } else if (s_connecting) {
            if (s_retry_num < MAX_STA_RETRY) {
                s_retry_num++;
                ESP_LOGI(TAG, "retry connect (%d/%d)", s_retry_num, MAX_STA_RETRY);
                esp_wifi_connect();
            } else if (s_wifi_events) {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_got_ip, sizeof(s_got_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        if (s_connecting && s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t netprov_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  STA connect with scan + candidate selection                       */
/* ------------------------------------------------------------------ */
static esp_err_t try_single_ssid(const char *ssid, const char *pass,
                                 char *ip_out, int timeout_ms)
{
    s_wifi_events = xEventGroupCreate();
    s_retry_num = 0;
    s_connecting = true;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    esp_err_t result;
    if (bits & WIFI_CONNECTED_BIT) {
        strlcpy(ip_out, s_got_ip, 16);
        strlcpy(s_connected_ip, s_got_ip, sizeof(s_connected_ip));
        ESP_LOGI(TAG, "connected to '%s', ip=%s", ssid, ip_out);
        s_connected = true;
        result = ESP_OK;
    } else {
        ESP_LOGW(TAG, "connect to '%s' failed", ssid);
        esp_wifi_stop();
        result = ESP_FAIL;
    }

    s_connecting = false;
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    return result;
}

esp_err_t netprov_try_connect(const struct netprov_config *cfg,
                               char *ip_out, int timeout_ms)
{
    s_connected = false;

    /* 1. Scan with retries */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Add a small delay for hardware initialization
    vTaskDelay(pdMS_TO_TICKS(100));

    uint16_t ap_count = 0;
    wifi_ap_record_t *records = NULL;
    int scan_retries = 3;
    int n_cands = 0;

    typedef struct { int slot; int rssi; wifi_ap_record_t rec; } cand_t;
    cand_t cands[NETPROV_MAX_SSID_SLOTS];

    for (int attempt = 1; attempt <= scan_retries; attempt++) {
        wifi_scan_config_t scan_cfg = { .show_hidden = false };
        esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
        if (scan_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi scan failed (err=0x%x), retrying scan (%d/%d)", scan_err, attempt, scan_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 32) ap_count = 32;

        records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (records && ap_count) {
            esp_wifi_scan_get_ap_records(&ap_count, records);
        }

        /* Build candidates: strongest matching SSID first */
        n_cands = 0;
        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            if (cfg->wifi[i].ssid[0] == '\0') continue;
            int best_rssi = -128;
            wifi_ap_record_t best_rec = {0};
            for (int j = 0; j < ap_count; j++) {
                if (records && strcmp((char *)records[j].ssid, cfg->wifi[i].ssid) == 0
                    && records[j].rssi > best_rssi) {
                    best_rssi = records[j].rssi;
                    best_rec = records[j];
                }
            }
            if (best_rssi > -128) {
                cands[n_cands].slot = i;
                cands[n_cands].rssi = best_rssi;
                cands[n_cands].rec = best_rec;
                n_cands++;
            }
        }

        if (records) {
            free(records);
            records = NULL;
        }

        if (n_cands > 0) {
            break; // Found candidate SSID(s)
        }

        if (attempt < scan_retries) {
            ESP_LOGI(TAG, "SSID candidates not found in scan, retrying scan in 1s (%d/%d)...", attempt, scan_retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    esp_wifi_stop();

    if (n_cands == 0) {
        ESP_LOGW(TAG, "no configured SSID visible after scan retries");
        return ESP_FAIL;
    }

    /* Sort by RSSI descending (bubble, small N) */
    for (int i = 0; i < n_cands - 1; i++) {
        for (int j = i + 1; j < n_cands; j++) {
            if (cands[j].rssi > cands[i].rssi) {
                cand_t t = cands[i]; cands[i] = cands[j]; cands[j] = t;
            }
        }
    }

    /* 3. Try each candidate: 3 attempts, 5 s between retries */
    for (int i = 0; i < n_cands; i++) {
        int slot = cands[i].slot;
        ESP_LOGI(TAG, "trying candidate %d: '%s' (%d dBm)",
                 i + 1, cfg->wifi[slot].ssid, cands[i].rssi);

        for (int attempt = 1; attempt <= MAX_STA_RETRY; attempt++) {
            esp_err_t err = try_single_ssid(cfg->wifi[slot].ssid,
                                            cfg->wifi[slot].pass, ip_out,
                                            timeout_ms);
            if (err == ESP_OK) return ESP_OK;
            if (attempt < MAX_STA_RETRY) {
                ESP_LOGI(TAG, "waiting 5 s before retry %d/%d",
                         attempt + 1, MAX_STA_RETRY);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
    }

    ESP_LOGW(TAG, "all candidates exhausted");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  HTTP helpers                                                      */
/* ------------------------------------------------------------------ */
static int url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_size; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return (int)di;
}

static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char raw[160];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(raw, out, out_size);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Web pages                                                         */
/* ------------------------------------------------------------------ */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<link rel=\"manifest\" href=\"/manifest.json\">"
"<title>SomnoTrace Portal</title><style>"
":root{--bg-color:#f1f5f9;--card-bg:#f8fafc;--text-color:#0f172a;--text-muted:#64748b;--border-color:#e2e8f0;--accent-color:#10b981;--accent-hover:#059669;--btn-sec-bg:#e2e8f0;--btn-sec-text:#0f172a;--btn-sec-hover:#cbd5e1;--glow-color:rgba(16,185,129,0.2)}"
"[data-theme=\"dark\"]{--bg-color:#0f172a;--card-bg:#1e293b;--text-color:#f1f5f9;--text-muted:#94a3b8;--border-color:#334155;--btn-sec-bg:#334155;--btn-sec-text:#f1f5f9;--btn-sec-hover:#475569;--glow-color:rgba(16,185,129,0.4)}"
"body{margin:0;padding:0;background-color:var(--bg-color);color:var(--text-color);font-family:system-ui,-apple-system,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;transition:background-color .3s,color .3s}"
".card{max-width:400px;width:100%;margin:16px;background-color:var(--card-bg);border:1px solid var(--border-color);border-radius:16px;padding:28px;box-shadow:0 10px 15px -3px rgba(0,0,0,0.1);box-sizing:border-box}"
"header{display:flex;justify-content:space-between;align-items:center;margin-bottom:24px}"
".logo{font-size:1.3rem;font-weight:800;color:var(--accent-color)}"
".theme-toggle{display:flex;background-color:var(--btn-sec-bg);border-radius:8px;padding:2px}"
".theme-toggle button{background:none;border:none;color:var(--text-muted);padding:4px 8px;border-radius:6px;cursor:pointer;font-size:0.75rem;font-weight:600}"
".theme-toggle button.active{background-color:var(--card-bg);color:var(--text-color);box-shadow:0 1px 3px rgba(0,0,0,0.1)}"
"h1{font-size:1.4rem;margin:0 0 6px 0;font-weight:700}"
"p{color:var(--text-muted);font-size:0.9rem;margin:0 0 20px 0}"
".form-group{margin-bottom:16px}"
"label{display:block;font-size:0.8rem;font-weight:700;margin-bottom:6px;text-transform:uppercase;color:var(--text-muted)}"
"select,input{width:100%;padding:10px 12px;border-radius:8px;border:1px solid var(--border-color);background-color:var(--card-bg);color:var(--text-color);font-size:0.9rem;box-sizing:border-box;outline:none}"
"select:focus,input:focus{border-color:var(--accent-color);box-shadow:0 0 0 2px var(--glow-color)}"
"button.primary{width:100%;padding:12px;border-radius:8px;border:none;background-color:var(--accent-color);color:#04210f;font-size:0.95rem;font-weight:700;cursor:pointer}"
"button.primary:hover{background-color:var(--accent-hover)}"
"button.secondary{width:100%;padding:10px;border-radius:8px;border:1px solid var(--border-color);background-color:var(--btn-sec-bg);color:var(--btn-sec-text);font-size:0.85rem;font-weight:600;cursor:pointer;margin-bottom:12px}"
"button.secondary:hover{background-color:var(--btn-sec-hover)}"
".status-msg{margin-top:12px;text-align:center;font-weight:600;font-size:0.85rem;min-height:1.2em;color:#fbbf24}"
".connected-view{text-align:center;display:flex;flex-direction:column;align-items:center}"
".status-icon{width:48px;height:48px;background-color:var(--glow-color);border-radius:50%;display:flex;align-items:center;justify-content:center;margin-bottom:16px;position:relative}"
".status-icon::after{content:'';width:16px;height:16px;background-color:var(--accent-color);border-radius:50%;animation:pulse 2s infinite}"
"@keyframes pulse{0%{transform:scale(0.95);box-shadow:0 0 0 0 var(--glow-color)}70%{transform:scale(1);box-shadow:0 0 0 8px rgba(16,185,129,0)}100%{transform:scale(0.95);box-shadow:0 0 0 0 rgba(16,185,129,0)}}"
".ip-badge{font-family:monospace;font-size:1.1rem;background-color:var(--btn-sec-bg);color:var(--text-color);padding:6px 12px;border-radius:6px;margin-top:8px;border:1px solid var(--border-color)}"
".wifi-block{border:1px solid var(--border-color);border-radius:12px;padding:16px;margin-bottom:16px;background-color:var(--bg-color);position:relative}"
".block-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
".block-title{font-size:0.75rem;font-weight:700;color:var(--text-muted);text-transform:uppercase}"
".btn-remove{background:none;border:none;color:#ef4444;font-size:0.8rem;font-weight:600;cursor:pointer;padding:0}"
".btn-remove:hover{text-decoration:underline}"
".btn-toggle-input{background:none;border:none;color:var(--accent-color);font-size:0.75rem;font-weight:600;cursor:pointer;padding:0}"
".btn-toggle-input:hover{text-decoration:underline}"
".btn-pass-toggle{position:absolute;right:8px;top:50%;transform:translateY(-50%);background:none;border:none;cursor:pointer;font-size:1.1rem;padding:4px;color:var(--text-muted);outline:none}"
".btn-add-container{margin-bottom:20px}"
"button.add-btn{width:100%;padding:10px;border-radius:8px;border:1px dashed var(--border-color);background:none;color:var(--text-color);font-size:0.85rem;font-weight:600;cursor:pointer;transition:border-color 0.2s,background-color 0.2s}"
"button.add-btn:hover{border-color:var(--accent-color);background-color:var(--glow-color)}"
"</style></head><body>"
"<div class=\"card\">"
"<header><div class=\"logo\">SomnoTrace</div>"
"<div class=\"theme-toggle\">"
"<button id=\"btn-theme-day\" onclick=\"applyTheme('day')\">Day</button>"
"<button id=\"btn-theme-night\" onclick=\"applyTheme('night')\">Night</button>"
"<button id=\"btn-theme-auto\" onclick=\"applyTheme('auto')\">Auto</button>"
"</div></header>"
"<div id=\"view-setup\" style=\"display:none\">"
"<h1>Setup Wi-Fi</h1><p>Configure up to 4 Wi-Fi networks.</p>"
"<button class=\"secondary\" onclick=\"scan()\">Scan for networks</button>"
"<div id=\"wifi-blocks\"></div>"
"<div class=\"btn-add-container\">"
"<button id=\"btn-add-wifi\" type=\"button\" class=\"add-btn\" onclick=\"addBlock()\">+ Add Another Wi-Fi Network</button>"
"</div>"
"<div class=\"form-group\">"
"<label>Timezone (GMT offset)</label>"
"<select id=\"gmt-off\"><option value=\"0\">UTC (GMT+0)</option></select>"
"</div>"
"<button class=\"primary\" onclick=\"save()\">Save &amp; Reboot</button>"
"<button id=\"btn-back-status\" class=\"secondary\" style=\"margin-top:12px;display:none\" onclick=\"showConnected()\">Back to Status</button>"
"<div id=\"status\" class=\"status-msg\"></div>"
"</div>"
"<div id=\"view-connected\" style=\"display:none\" class=\"connected-view\">"
"<div class=\"status-icon\"></div>"
"<h1>Connected</h1><p>The device has connected to the Wi-Fi network. You can reach it at:</p>"
"<div id=\"ip-addr\" class=\"ip-badge\">0.0.0.0</div>"
"<button class=\"secondary\" style=\"margin-top:24px\" onclick=\"showSetup()\">Configure Wi-Fi / Add Networks</button>"
"<div class=\"wifi-block\" style=\"margin-top:20px;text-align:left\">"
"<div class=\"block-header\"><span class=\"block-title\">CPAP Device</span>"
"<button type=\"button\" id=\"btn-ble-forget\" class=\"btn-remove\" style=\"display:none\" onclick=\"bleForget()\">Forget</button></div>"
"<div id=\"ble-paired\" style=\"display:none\">"
"<p style=\"margin:0 0 10px 0\">Paired with <strong id=\"ble-dev-name\"></strong></p></div>"
"<div id=\"ble-unpaired\">"
"<button type=\"button\" class=\"secondary\" onclick=\"bleScan()\">Scan for AirSense 11</button>"
"<div class=\"form-group\" id=\"ble-select-group\" style=\"display:none\">"
"<label>Device</label><select id=\"ble-dev\"></select></div>"
"<button type=\"button\" id=\"btn-ble-pair\" class=\"primary\" style=\"display:none\" onclick=\"blePair()\">Pair</button>"
"<div class=\"form-group\" id=\"ble-passkey-group\" style=\"display:none;margin-top:12px\">"
"<label>4-digit code shown on the device</label>"
"<input id=\"ble-passkey\" type=\"text\" inputmode=\"numeric\" maxlength=\"4\" placeholder=\"1234\">"
"<button type=\"button\" class=\"primary\" style=\"margin-top:10px\" onclick=\"bleConfirm()\">Confirm</button></div>"
"</div>"
"<div id=\"ble-status\" class=\"status-msg\"></div>"
"</div>"
"</div>"
"</div>"
"<script>"
"var activeBlocks = 1;"
"var manualSSID = {};"
"var passRevealed = {};"
"var scannedNetworks = [];"
"function initTheme(){applyTheme(localStorage.getItem('theme-mode')||'auto')}"
"function applyTheme(m){"
"const r=document.documentElement;let t='light';"
"if(m==='night')t='dark';"
"else if(m==='auto'){t=(new Date().getHours()>=8&&new Date().getHours()<20)?'light':'dark'}"
"r.setAttribute('data-theme',t);localStorage.setItem('theme-mode',m);"
"['day','night','auto'].forEach(x=>{const b=document.getElementById('btn-theme-'+x);"
"if(b){if(x===m)b.classList.add('active');else b.classList.remove('active')}})}"
"function populateGmtOff(saved){"
"var sel=document.getElementById('gmt-off');if(!sel)return;"
"sel.innerHTML='';"
"for(var i=-12;i<=14;i++){"
"var o=document.createElement('option');"
"o.value=i;"
"o.textContent='GMT'+(i>=0?'+':'')+i;"
"if(i===saved)o.selected=true;"
"sel.appendChild(o)}}"
"function loadStatus(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"const s=document.getElementById('view-setup'),c=document.getElementById('view-connected');"
"const isWifiPath = window.location.pathname === '/wifi';"
"const backBtn = document.getElementById('btn-back-status');"
"if(d.mode==='connected'){if(backBtn)backBtn.style.display='block'}else{if(backBtn)backBtn.style.display='none'}"
"populateGmtOff(d.gmt_off||0);"
"if(d.ssids && d.ssids.length > 0){"
"activeBlocks = d.ssids.length;"
"for(let i=1;i<=d.ssids.length;i++){manualSSID[i]=true}"
"}"
"renderBlocks();"
"if(d.ssids && d.ssids.length > 0){"
"for(let i=1;i<=d.ssids.length;i++){"
"const el=document.getElementById('ssid-'+i);"
"if(el)el.value=d.ssids[i-1];"
"}"
"}"
"if(d.mode==='setup' || isWifiPath){"
"s.style.display='block';c.style.display='none';scan()}"
"else{"
"s.style.display='none';c.style.display='block';document.getElementById('ip-addr').textContent=d.ip}"
"}).catch(()=>{renderBlocks();document.getElementById('view-setup').style.display='block';scan()})}"
"function scan(){"
"const s=document.getElementById('status');s.textContent='Scanning...';"
"fetch('/scan').then(r=>r.json()).then(d=>{"
"const unique={};"
"d.forEach(n=>{if(!unique[n.ssid]||unique[n.ssid].rssi<n.rssi){unique[n.ssid]=n}});"
"scannedNetworks=Object.values(unique).sort((a,b)=>b.rssi-a.rssi);"
"s.textContent=scannedNetworks.length+' networks found';"
"for(let i=1;i<=activeBlocks;i++){if(!manualSSID[i])populateSelect(i)}"
"}).catch(()=>{s.textContent='Scan failed'})"
"}"
"function populateSelect(id){"
"const sel=document.getElementById('ssid-'+id);if(!sel)return;"
"const currentVal=sel.value;"
"sel.innerHTML='<option value=\"\">-- select network --</option>';"
"scannedNetworks.forEach(n=>{"
"const o=document.createElement('option');o.value=n.ssid;"
"o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.lock?' 🔒':'');"
"if(n.ssid===currentVal)o.selected=true;"
"sel.appendChild(o)});"
"}"
"function renderBlocks(){"
"const container=document.getElementById('wifi-blocks');"
"const savedVals=[];"
"for(let i=1;i<=4;i++){"
"const sEl=document.getElementById('ssid-'+i),pEl=document.getElementById('pass-'+i);"
"savedVals.push({ssid:sEl?sEl.value:'',pass:pEl?pEl.value:''})}"
"container.innerHTML='';"
"for(let i=1;i<=activeBlocks;i++){"
"const block=document.createElement('div');block.className='wifi-block';"
"block.innerHTML='<div class=\"block-header\"><span class=\"block-title\">Wi-Fi Network #'+i+'</span>'+"
"(i>1?'<button type=\"button\" class=\"btn-remove\" onclick=\"removeBlock('+i+')\">Remove</button>':'')+'</div>'+"
"'<div class=\"form-group\"><div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:6px\">'+"
"'<label>SSID</label><button type=\"button\" class=\"btn-toggle-input\" id=\"btn-manual-'+i+'\" onclick=\"toggleManualSSID('+i+')\">Enter Manually</button></div>'+"
"'<div id=\"ssid-container-'+i+'\"></div></div>'+"
"'<div class=\"form-group\"><label>Password</label><div style=\"display:flex;position:relative\">'+"
"'<input id=\"pass-'+i+'\" type=\"'+(passRevealed[i]?'text':'password')+'\" placeholder=\"Password\" style=\"padding-right:45px\">'+"
"'<button type=\"button\" class=\"btn-pass-toggle\" onclick=\"togglePass('+i+')\">'+(passRevealed[i]?'🙈':'👁️')+'</button></div></div>';"
"container.appendChild(block);"
"const sCont=document.getElementById('ssid-container-'+i);"
"if(manualSSID[i]){"
"sCont.innerHTML='<input id=\"ssid-'+i+'\" type=\"text\" placeholder=\"Enter custom SSID\">';"
"document.getElementById('btn-manual-'+i).textContent='Select Scanned'}"
"else{"
"sCont.innerHTML='<select id=\"ssid-'+i+'\"><option value=\"\">-- select network --</option></select>';"
"document.getElementById('btn-manual-'+i).textContent='Enter Manually';"
"populateSelect(i)}"
"const sEl=document.getElementById('ssid-'+i),pEl=document.getElementById('pass-'+i);"
"if(sEl&&savedVals[i-1])sEl.value=savedVals[i-1].ssid;"
"if(pEl&&savedVals[i-1])pEl.value=savedVals[i-1].pass}"
"const addBtn=document.getElementById('btn-add-wifi');"
"if(addBtn)addBtn.style.display=(activeBlocks<4)?'block':'none'"
"}"
"function addBlock(){if(activeBlocks<4){activeBlocks++;renderBlocks()}}"
"function removeBlock(idx){"
"for(let i=idx;i<activeBlocks;i++){"
"const nextSSID=document.getElementById('ssid-'+(i+1)),nextPass=document.getElementById('pass-'+(i+1));"
"const currSSID=document.getElementById('ssid-'+i),currPass=document.getElementById('pass-'+i);"
"if(currSSID&&nextSSID)currSSID.value=nextSSID.value;"
"if(currPass&&nextPass)currPass.value=nextPass.value;"
"manualSSID[i]=manualSSID[i+1];passRevealed[i]=passRevealed[i+1]}"
"activeBlocks--;renderBlocks()}"
"function toggleManualSSID(id){manualSSID[id]=!manualSSID[id];renderBlocks()}"
"function togglePass(id){passRevealed[id]=!passRevealed[id];renderBlocks()}"
"function showSetup(){"
"document.getElementById('view-setup').style.display='block';"
"document.getElementById('view-connected').style.display='none';"
"scan();"
"}"
"function showConnected(){"
"document.getElementById('view-setup').style.display='none';"
"document.getElementById('view-connected').style.display='block';"
"}"
"function save(){"
"const s=document.getElementById('status');let body='';"
"let validCount=0;"
"for(let i=1;i<=activeBlocks;i++){"
"const ssidEl=document.getElementById('ssid-'+i),passEl=document.getElementById('pass-'+i);"
"const ssid=ssidEl?ssidEl.value.trim():'',pass=passEl?passEl.value:'';"
"if(ssid){"
"if(body)body+='&';"
"body+='ssid'+i+'='+encodeURIComponent(ssid)+'&pass'+i+'='+encodeURIComponent(pass);"
"validCount++"
"}"
"}"
"if(validCount===0){s.textContent='Please configure at least one network';return}"
"var gmtEl=document.getElementById('gmt-off');"
"if(gmtEl)body+='&gmt_off='+gmtEl.value;"
"s.textContent='Saving...';"
"fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
".then(r=>r.text()).then(()=>{s.textContent='Saved. Rebooting...';})"
".catch(()=>{s.textContent='Save failed'})"
"}"
"var blePollTimer=null;"
"function bleStatusMsg(t){document.getElementById('ble-status').textContent=t}"
"function bleScan(){bleStatusMsg('Scanning for AirSense 11...');"
"fetch('/api/ble/scan').then(r=>r.json()).then(d=>{"
"var sel=document.getElementById('ble-dev');sel.innerHTML='';"
"if(!d.length){bleStatusMsg('No AirSense 11 devices found');return}"
"d.sort((a,b)=>b.rssi-a.rssi);d.forEach(n=>{var o=document.createElement('option');"
"o.value=n.addr;o.textContent=n.name+' ('+n.rssi+' dBm)';sel.appendChild(o)});"
"document.getElementById('ble-select-group').style.display='block';"
"document.getElementById('btn-ble-pair').style.display='block';"
"bleStatusMsg(d.length+' device(s) found')}).catch(()=>bleStatusMsg('Scan failed'))}"
"function blePair(){var addr=document.getElementById('ble-dev').value;if(!addr){bleStatusMsg('Pick a device');return}"
"bleStatusMsg('Connecting...');"
"fetch('/api/ble/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr:addr})})"
".then(r=>r.json()).then(()=>blePoll()).catch(()=>bleStatusMsg('Pair failed'))}"
"function bleConfirm(){var pk=document.getElementById('ble-passkey').value.trim();"
"if(pk.length!==4){bleStatusMsg('Enter the 4-digit code');return}"
"bleStatusMsg('Verifying...');"
"fetch('/api/ble/confirm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({passkey:pk})})"
".then(r=>r.json()).then(()=>blePoll()).catch(()=>bleStatusMsg('Confirm failed'))}"
"function bleForget(){if(!confirm('Forget the paired CPAP device?'))return;"
"fetch('/api/ble/forget',{method:'POST'}).then(()=>bleRefresh())}"
"function blePoll(){if(blePollTimer)clearInterval(blePollTimer);"
"blePollTimer=setInterval(bleRefresh,500);bleRefresh()}"
"function bleRefresh(){fetch('/api/ble/status').then(r=>r.json()).then(d=>{"
"var paired=document.getElementById('ble-paired'),unp=document.getElementById('ble-unpaired'),"
"fgt=document.getElementById('btn-ble-forget'),pg=document.getElementById('ble-passkey-group');"
"if(d.state==='waiting_passkey'){pg.style.display='block';bleStatusMsg('Enter the code shown on the device screen');return}"
"if(d.state==='connecting'){bleStatusMsg('Connecting to device...');return}"
"if(d.state==='confirming'){bleStatusMsg('Verifying code...');return}"
"if(d.state==='error'){if(blePollTimer)clearInterval(blePollTimer);pg.style.display='none';bleStatusMsg('Error: '+(d.error||'pairing failed'));return}"
"if(d.paired){if(blePollTimer)clearInterval(blePollTimer);"
"paired.style.display='block';unp.style.display='none';fgt.style.display='block';pg.style.display='none';"
"document.getElementById('ble-dev-name').textContent=(d.device&&d.device.name)||'AirSense 11';"
"bleStatusMsg(d.state==='paired'?'Paired successfully':'')}"
"else{paired.style.display='none';unp.style.display='block';fgt.style.display='none'}"
"}).catch(()=>{})}"
"window.onload=()=>{initTheme();loadStatus();bleRefresh();if('serviceWorker' in navigator)navigator.serviceWorker.register('/sw.js')};"
"</script></body></html>";

static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const char manifest[] =
        "{\n"
        "  \"short_name\": \"SomnoTrace\",\n"
        "  \"name\": \"SomnoTrace Web Portal\",\n"
        "  \"start_url\": \"/\",\n"
        "  \"background_color\": \"#0f172a\",\n"
        "  \"theme_color\": \"#0f172a\",\n"
        "  \"display\": \"standalone\",\n"
        "  \"orientation\": \"any\",\n"
        "  \"icons\": [\n"
        "    {\n"
        "      \"src\": \"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='%2310b981'/></svg>\",\n"
        "      \"sizes\": \"192x192\",\n"
        "      \"type\": \"image/svg+xml\"\n"
        "    }\n"
        "  ]\n"
        "}";
    httpd_resp_send(req, manifest, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sw_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    const char sw[] =
        "const CACHE_NAME = 'somnotrace-v1';\n"
        "self.addEventListener('install', e => {\n"
        "  e.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(['/', '/manifest.json'])));\n"
        "});\n"
        "self.addEventListener('fetch', e => {\n"
        "  if (e.request.url.includes('/api/') || e.request.url.includes('/scan') || e.request.url.includes('/save')) {\n"
        "    e.respondWith(fetch(e.request));\n"
        "  } else {\n"
        "    e.respondWith(caches.match(e.request).then(res => res || fetch(e.request)));\n"
        "  }\n"
        "});\n";
    httpd_resp_send(req, sw, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    struct netprov_config cfg;
    bool has_cfg = netprov_load_config(&cfg);

    char ssids_json[256] = "";
    int added = 0;
    if (has_cfg) {
        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            if (cfg.wifi[i].ssid[0] != '\0') {
                char item[80];
                snprintf(item, sizeof(item), "%s\"%s\"", added > 0 ? "," : "", cfg.wifi[i].ssid);
                strlcat(ssids_json, item, sizeof(ssids_json));
                added++;
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    int gmt = time_sync_get_gmt_offset();
    bool synced = time_sync_is_synced();
    char time_str[32] = "";
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(time_str, sizeof(time_str), "\"%Y-%m-%dT%H:%M:%S\"", &tm_info);
    } else {
        strlcpy(time_str, "null", sizeof(time_str));
    }

    int rssi = -128;
    uint8_t primary_chan = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_sta_get_rssi(&rssi);
    esp_wifi_get_channel(&primary_chan, &second_chan);

    char resp[512];
    if (s_portal_mode) {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"setup\",\"ssids\":[%s],\"gmt_off\":%d,\"time\":%s,\"ntp_synced\":%s,"
                 "\"rssi\":%d,\"channel\":%d}",
                 ssids_json, gmt, time_str, synced ? "true" : "false",
                 rssi, primary_chan);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"connected\",\"ip\":\"%s\",\"ssids\":[%s],\"gmt_off\":%d,\"time\":%s,\"ntp_synced\":%s,"
                 "\"rssi\":%d,\"channel\":%d}",
                 s_connected_ip, ssids_json, gmt, time_str, synced ? "true" : "false",
                 rssi, primary_chan);
    }
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    cJSON *arr = cJSON_CreateArray();
    if (records && ap_count) {
        esp_wifi_scan_get_ap_records(&ap_count, records);
        for (int i = 0; i < ap_count; i++) {
            if (records[i].ssid[0] == '\0') continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (char *)records[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", records[i].rssi);
            cJSON_AddBoolToObject(o, "lock", records[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(records);

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  BLE (AirSense 11) pairing endpoints                               */
/* ------------------------------------------------------------------ */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len < (int)cap - 1 ? req->content_len : (int)cap - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t ble_scan_handler(httpd_req_t *req)
{
    if (as11_ble_scan(6) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ble not ready");
        return ESP_FAIL;
    }
    cJSON *arr = as11_ble_get_scan_results();
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t ble_pair_handler(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "addr") : NULL;
    if (!cJSON_IsString(addr)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr");
        return ESP_FAIL;
    }
    esp_err_t e = as11_ble_start_pair(addr->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair start failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ble_confirm_handler(httpd_req_t *req)
{
    char body[96];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *pk = j ? cJSON_GetObjectItem(j, "passkey") : NULL;
    if (!cJSON_IsString(pk)) {
        if (j) cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing passkey");
        return ESP_FAIL;
    }
    esp_err_t e = as11_ble_confirm_pair(pk->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "confirm failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t ble_status_handler(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", as11_ble_get_status());
    cJSON_AddStringToObject(o, "error", as11_ble_get_error());
    cJSON_AddBoolToObject(o, "paired", as11_ble_is_paired());
    cJSON *info = as11_ble_get_paired_info();
    if (info) cJSON_AddItemToObject(o, "device", info);
    char *json = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t ble_forget_handler(httpd_req_t *req)
{
    as11_ble_forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "rebooting to apply credentials");
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[768];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    struct netprov_config cfg;
    netprov_load_config(&cfg);
    memset(cfg.wifi, 0, sizeof(cfg.wifi));

    int saved_count = 0;
    for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
        char ssid_key[16];
        char pass_key[16];
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i + 1);
        snprintf(pass_key, sizeof(pass_key), "pass%d", i + 1);

        char ssid[NETPROV_SSID_MAXLEN + 1] = { 0 };
        char pass[NETPROV_PASS_MAXLEN + 1] = { 0 };

        if (form_get(body, ssid_key, ssid, sizeof(ssid)) && ssid[0] != '\0') {
            form_get(body, pass_key, pass, sizeof(pass));
            strlcpy(cfg.wifi[saved_count].ssid, ssid, sizeof(cfg.wifi[saved_count].ssid));
            strlcpy(cfg.wifi[saved_count].pass, pass, sizeof(cfg.wifi[saved_count].pass));
            saved_count++;
        }
    }

    if (saved_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }

    if (netprov_save_config(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %d credentials", saved_count);

    /* Save GMT offset if present */
    char gmt_str[8] = { 0 };
    if (form_get(body, "gmt_off", gmt_str, sizeof(gmt_str)) && gmt_str[0] != '\0') {
        int gmt = atoi(gmt_str);
        if (gmt >= -12 && gmt <= 14) {
            time_sync_set_gmt_offset(gmt);
            ESP_LOGI(TAG, "saved GMT offset %d", gmt);
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style=\"font-family:sans-serif\">Saved. Rebooting to connect...</body></html>");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  EZShare-compatible file server (/dir, /download)                  */
/* ------------------------------------------------------------------ */

#include <dirent.h>
#include <sys/stat.h>

#define SD_ROOT "/somnotrace"

/* Check if path contains ".." (traversal protection) */
static bool path_is_safe(const char *path)
{
    if (!path) return false;
    if (strstr(path, "..")) return false;
    return true;
}

/* URL-decode a query parameter value in-place */
static int fs_url_decode(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = src[1] >= 'A' ? (src[1] | 0x20) - 'a' + 10 : src[1] - '0';
            int lo = src[2] >= 'A' ? (src[2] | 0x20) - 'a' + 10 : src[2] - '0';
            dst[i++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return i;
}

/* Extract a query parameter from the URI query string */
static bool get_query_param(httpd_req_t *req, const char *key, char *out, int out_len)
{
    char buf[512];
    int len = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (len <= 0) return false;

    char key_eq[32];
    snprintf(key_eq, sizeof(key_eq), "%s=", key);

    char *p = strstr(buf, key_eq);
    if (!p) return false;
    p += strlen(key_eq);

    char *end = strchr(p, '&');
    int val_len = end ? (int)(end - p) : (int)strlen(p);
    if (val_len <= 0) return false;

    char raw[256];
    if (val_len >= (int)sizeof(raw)) val_len = sizeof(raw) - 1;
    memcpy(raw, p, val_len);
    raw[val_len] = '\0';

    fs_url_decode(out, raw, out_len);
    return true;
}

static esp_err_t dir_get_handler(httpd_req_t *req)
{
    char dir_path[256];
    if (!get_query_param(req, "dir", dir_path, sizeof(dir_path)) || dir_path[0] == '\0') {
        strlcpy(dir_path, "/", sizeof(dir_path));
    }

    if (!path_is_safe(dir_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "dir not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    /* Build HTML <pre> listing — heap-allocated to avoid stack overflow */
    char *html = malloc(4096);
    if (!html) {
        closedir(d);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int pos = 0;
    pos += snprintf(html + pos, 4096 - pos, "<html><body><pre>\n");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && pos < 4096 - 128) {
        if (ent->d_name[0] == '.') continue;

        char full_path[530];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        char timestr[32];
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm);

        if (S_ISDIR(st.st_mode)) {
            pos += snprintf(html + pos, 4096 - pos,
                "%s    &lt;DIR&gt;    <a href=\"/dir?dir=%s/%s\">%s</a>\n",
                timestr, dir_path, ent->d_name, ent->d_name);
        } else {
            pos += snprintf(html + pos, 4096 - pos,
                "%s    %8ld    <a href=\"/download?path=%s/%s\">%s</a>\n",
                timestr, (long)st.st_size, dir_path, ent->d_name, ent->d_name);
        }
    }
    closedir(d);

    pos += snprintf(html + pos, 4096 - pos, "</pre></body></html>\n");
    httpd_resp_send(req, html, pos);
    free(html);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char file_path[256];
    if (!get_query_param(req, "path", file_path, sizeof(file_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_FAIL;
    }

    if (!path_is_safe(file_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(file_path, &st) == 0) {
        char len_str[16];
        snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", len_str);
    }

    httpd_resp_set_type(req, "application/octet-stream");

    /* Heap-allocate to avoid stack overflow */
    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t n;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_webserver(void)
{
    if (s_httpd) {
        ESP_LOGI(TAG, "stopping existing webserver");
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 26;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd");
        return ESP_FAIL;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_httpd, &root);

    httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_httpd, &wifi_uri);

    httpd_uri_t manifest = { .uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_get_handler };
    httpd_register_uri_handler(s_httpd, &manifest);

    httpd_uri_t sw = { .uri = "/sw.js", .method = HTTP_GET, .handler = sw_get_handler };
    httpd_register_uri_handler(s_httpd, &sw);

    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_httpd, &status);

    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);

    /* AirSense 11 BLE pairing endpoints */
    httpd_uri_t ble_scan = { .uri = "/api/ble/scan", .method = HTTP_GET, .handler = ble_scan_handler };
    httpd_uri_t ble_pair = { .uri = "/api/ble/pair", .method = HTTP_POST, .handler = ble_pair_handler };
    httpd_uri_t ble_conf = { .uri = "/api/ble/confirm", .method = HTTP_POST, .handler = ble_confirm_handler };
    httpd_uri_t ble_stat = { .uri = "/api/ble/status", .method = HTTP_GET, .handler = ble_status_handler };
    httpd_uri_t ble_forget = { .uri = "/api/ble/forget", .method = HTTP_POST, .handler = ble_forget_handler };
    httpd_register_uri_handler(s_httpd, &ble_scan);
    httpd_register_uri_handler(s_httpd, &ble_pair);
    httpd_register_uri_handler(s_httpd, &ble_conf);
    httpd_register_uri_handler(s_httpd, &ble_stat);
    httpd_register_uri_handler(s_httpd, &ble_forget);

    /* EZShare-compatible file server endpoints */
    httpd_uri_t dir_hdl = { .uri = "/dir", .method = HTTP_GET, .handler = dir_get_handler };
    httpd_uri_t dl_hdl = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler };
    httpd_register_uri_handler(s_httpd, &dir_hdl);
    httpd_register_uri_handler(s_httpd, &dl_hdl);

    if (s_portal_mode) {
        /* Captive-portal probe intercepts (return 302 to trigger portal popup) */
        const char *probes[] = {
            "/hotspot-detect.html",
            "/generate_204",
            "/connecttest.txt",
            "/ncsi.txt",
            "/success.txt",
            NULL,
        };
        for (int i = 0; probes[i]; i++) {
            httpd_uri_t probe = {
                .uri = probes[i],
                .method = HTTP_GET,
                .handler = redirect_to_portal,
            };
            httpd_register_uri_handler(s_httpd, &probe);
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Captive DNS server (wildcard hijack)                              */
/* ------------------------------------------------------------------ */
void netprov_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    ESP_LOGI(TAG, "captive DNS server listening on port 53");

    while (true) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&src_addr, &src_len);
        if (len < 12) continue;

        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount != 1) continue;

        int qoff = 12;
        while (qoff < len && buf[qoff] != 0) {
            qoff += buf[qoff] + 1;
        }
        qoff++;
        if (qoff + 4 > len) continue;
        uint16_t qtype = (buf[qoff] << 8) | buf[qoff + 1];
        uint16_t qclass = (buf[qoff + 2] << 8) | buf[qoff + 3];
        qoff += 4;

        if (qtype != 1 || qclass != 1) continue;

        uint8_t resp[512];
        int rlen = 0;
        resp[rlen++] = buf[0]; resp[rlen++] = buf[1];
        resp[rlen++] = 0x81; resp[rlen++] = 0x80;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        memcpy(resp + rlen, buf + 12, qoff - 12);
        rlen += qoff - 12;

        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;
        resp[rlen++] = (s_ap_ip >> 0) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 8) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 16) & 0xFF;
        resp[rlen++] = (s_ap_ip >> 24) & 0xFF;

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&src_addr, src_len);
    }
}

/* ------------------------------------------------------------------ */
/*  Public start functions                                            */
/* ------------------------------------------------------------------ */
esp_err_t netprov_start_portal(const struct netprov_config *cfg, char *ap_ip_out)
{
    s_portal_mode = true;
    s_connected = false;
    s_connecting = false;
    esp_wifi_disconnect();
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-setup", cfg->hostname);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "",
            .ssid_len = strlen(s_ap_ssid),
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_cfg.ap.ssid_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_netif_ap, &ip_info);
    if (ap_ip_out) {
        snprintf(ap_ip_out, 16, IPSTR, IP2STR(&ip_info.ip));
    }
    s_ap_ip = ip_info.ip.addr;

    ESP_LOGI(TAG, "SoftAP '%s' up at " IPSTR, s_ap_ssid, IP2STR(&ip_info.ip));

    xTaskCreate(netprov_dns_task, "dns", 4096, NULL, 5, NULL);
    return start_webserver();
}

esp_err_t netprov_start_connected_server(const char *ip)
{
    s_portal_mode = false;
    strlcpy(s_connected_ip, ip, sizeof(s_connected_ip));
    return start_webserver();
}
