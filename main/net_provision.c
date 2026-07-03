/*
 * SomnoTrace - Wi-Fi provisioning, SoftAP captive portal, and NVS config
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include "net_provision.h"
#include "as11_ble.h"
#include "time_sync.h"
#include "uploader.h"
#include "edf_gen.h"
#include "sd_storage.h"

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
#include "esp_heap_caps.h"
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
extern const char _binary_portal_html_start[];
extern const char _binary_portal_html_end[];
#define PORTAL_HTML_START _binary_portal_html_start
#define PORTAL_HTML_LEN   ((size_t)(_binary_portal_html_end - _binary_portal_html_start))

extern const char _binary_zones_json_start[];
extern const char _binary_zones_json_end[];
#define ZONES_JSON_START _binary_zones_json_start
#define ZONES_JSON_LEN   ((size_t)(_binary_zones_json_end - _binary_zones_json_start))

/* portal.html is embedded via CMakeLists.txt target_add_binary_data */


static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (s_portal_mode) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML_START, PORTAL_HTML_LEN);
    return ESP_OK;
}

static esp_err_t tz_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ZONES_JSON_START, HTTPD_RESP_USE_STRLEN);
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
    char has_pass_json[64] = "";
    int added = 0;
    if (has_cfg) {
        for (int i = 0; i < NETPROV_MAX_SSID_SLOTS; i++) {
            if (cfg.wifi[i].ssid[0] != '\0') {
                char item[80];
                snprintf(item, sizeof(item), "%s\"%s\"", added > 0 ? "," : "", cfg.wifi[i].ssid);
                strlcat(ssids_json, item, sizeof(ssids_json));
                char hp[8];
                snprintf(hp, sizeof(hp), "%s%s", added > 0 ? "," : "", cfg.wifi[i].pass[0] ? "true" : "false");
                strlcat(has_pass_json, hp, sizeof(has_pass_json));
                added++;
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    char tz_str[64];
    char tz_name[40];
    time_sync_get_timezone(tz_str, sizeof(tz_str));
    time_sync_get_tz_name(tz_name, sizeof(tz_name));
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

    char resp[700];
    if (s_portal_mode) {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"setup\",\"ssids\":[%s],\"has_pass\":[%s],\"tz_name\":\"%s\",\"time\":%s,\"ntp_synced\":%s,"
                 "\"rssi\":%d,\"channel\":%d}",
                 ssids_json, has_pass_json, tz_name, time_str, synced ? "true" : "false",
                 rssi, primary_chan);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"connected\",\"ip\":\"%s\",\"ssids\":[%s],\"has_pass\":[%s],\"tz_name\":\"%s\",\"time\":%s,\"ntp_synced\":%s,"
                 "\"rssi\":%d,\"channel\":%d}",
                 s_connected_ip, ssids_json, has_pass_json, tz_name, time_str, synced ? "true" : "false",
                 rssi, primary_chan);
    }
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Async WiFi scan (non-blocking to avoid socket exhaustion)         */
/* ------------------------------------------------------------------ */
static volatile bool s_scan_running = false;
static volatile bool s_scan_done = false;
static char *s_scan_json = NULL;   /* cached JSON result */
static SemaphoreHandle_t s_scan_mutex = NULL;

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "wifi scan starting");
    if (s_portal_mode) {
        /* SoftAP: BLE is disconnected, so custom active scan params are safe.
         * ~20ms per channel × 13 channels ≈ 300ms total. */
        wifi_scan_config_t fast_cfg = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 0,
            .scan_time.active.max = 20,
        };
        esp_wifi_scan_start(&fast_cfg, true);
    } else {
        /* STA: BLE may be active — pass NULL to let the driver use
         * BT-coexistence-safe defaults. */
        esp_wifi_scan_start(NULL, true);
    }
    ESP_LOGI(TAG, "wifi scan complete");

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
    cJSON_Delete(arr);

    if (s_scan_mutex) xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (s_scan_json) cJSON_free(s_scan_json);
    s_scan_json = json;
    s_scan_done = true;
    s_scan_running = false;
    if (s_scan_mutex) xSemaphoreGive(s_scan_mutex);

    vTaskDelete(NULL);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();

    /* If a scan is running, tell the client to poll */
    if (s_scan_running) {
        httpd_resp_send(req, "{\"scanning\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* If we have cached results, return them */
    if (s_scan_done && s_scan_json) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        char *cached = s_scan_json;
        s_scan_json = NULL;
        s_scan_done = false;
        xSemaphoreGive(s_scan_mutex);
        httpd_resp_send(req, cached, HTTPD_RESP_USE_STRLEN);
        cJSON_free(cached);
        return ESP_OK;
    }

    /* Start a new scan in a background task */
    s_scan_running = true;
    s_scan_done = false;
    BaseType_t ret = xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 3, NULL);
    if (ret != pdPASS) {
        s_scan_running = false;
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"scanning\":true}", HTTPD_RESP_USE_STRLEN);
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

    /* Check if this is a timezone-only update */
    char tz_only[4] = { 0 };
    form_get(body, "tz_only", tz_only, sizeof(tz_only));
    bool is_tz_only = (tz_only[0] == '1');

    struct netprov_config old_cfg;
    netprov_load_config(&old_cfg);

    struct netprov_config cfg;
    memcpy(&cfg, &old_cfg, sizeof(cfg));

    int saved_count = 0;

    if (!is_tz_only) {
        memset(cfg.wifi, 0, sizeof(cfg.wifi));

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
                if (strcmp(pass, "\xe2\x96\x88UNCHANGED\xe2\x96\x88") == 0) {
                    for (int j = 0; j < NETPROV_MAX_SSID_SLOTS; j++) {
                        if (strcmp(old_cfg.wifi[j].ssid, ssid) == 0) {
                            strlcpy(cfg.wifi[saved_count].pass, old_cfg.wifi[j].pass, sizeof(cfg.wifi[saved_count].pass));
                            break;
                        }
                    }
                } else {
                    strlcpy(cfg.wifi[saved_count].pass, pass, sizeof(cfg.wifi[saved_count].pass));
                }
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
    }

    /* Save timezone if present */
    char tz_str_val[64] = { 0 };
    char tz_name_val[40] = { 0 };
    if (form_get(body, "tz_str", tz_str_val, sizeof(tz_str_val)) && tz_str_val[0] != '\0') {
        form_get(body, "tz_name", tz_name_val, sizeof(tz_name_val));
        time_sync_set_timezone(tz_str_val, tz_name_val);
        ESP_LOGI(TAG, "saved timezone %s (%s)", tz_name_val, tz_str_val);
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

/* ── Upload config and status endpoints ────────────────────────────── */

static esp_err_t upload_config_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (uploader_get_config_json(&json) != ESP_OK || !json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t upload_config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received < 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    if (uploader_save_config_json(body) != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
        return ESP_FAIL;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_status_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    if (uploader_get_status_json(&json) != ESP_OK || !json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* ── Actions: Reset State, Delete EDFs, Reset All, Recreate EDFs ───── */

/* Recursively delete a directory and all its contents. */
static void recursive_delete(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (ent->d_type == DT_DIR) {
            recursive_delete(child);
        } else {
            remove(child);
        }
    }
    closedir(d);
    rmdir(path);
}

/* Scan SDCARD/DATALOG for day folders and queue each for upload. */
static void scan_and_queue_uploads(void)
{
    DIR *d = opendir(SD_SDCARD_DATALOG);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type != DT_DIR) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* Each subdirectory is a noon-day folder — queue it. */
        uploader_on_day_ready(ent->d_name);
    }
    closedir(d);
}

/* Background task for recreate EDFs (long-running, needs large stack). */
typedef struct {
    char session_dir[256];
    char session_id[32];
    int64_t start_epoch_ms;
    int64_t end_epoch_ms;
    int64_t clock_drift_ms;
} recreate_session_t;

static void recreate_edfs_task(void *arg)
{
    ESP_LOGI(TAG, "recreate_edfs_task: starting");

    /* 1. Delete everything in SDCARD/ recursively. */
    recursive_delete(SD_SDCARD_DIR);
    ESP_LOGI(TAG, "recreate_edfs_task: SDCARD/ deleted");

    /* 2. Scan .sessions/streams/ for day folders, collect sessions.
     * Allocate the sessions array on the heap (PSRAM) — 64 * ~312 bytes
     * = ~20KB, which would overflow the 10KB task stack. */
    recreate_session_t *sessions = calloc(64, sizeof(recreate_session_t));
    if (!sessions) {
        ESP_LOGE(TAG, "recreate_edfs_task: failed to allocate sessions array");
        vTaskDelete(NULL);
        return;
    }
    int n_sessions = 0;

    DIR *d = opendir(SD_STREAMS_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && n_sessions < 64) {
            if (ent->d_type != DT_DIR) continue;
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char day_dir[300];
            snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_STREAMS_DIR, ent->d_name);
            DIR *dd = opendir(day_dir);
            if (!dd) continue;
            struct dirent *fent;
            while ((fent = readdir(dd)) != NULL && n_sessions < 64) {
                if (fent->d_type != DT_REG) continue;
                const char *suffix = "_session.json";
                int slen = strlen(suffix);
                int flen = strlen(fent->d_name);
                if (flen <= slen || strcmp(fent->d_name + flen - slen, suffix) != 0)
                    continue;
                /* Extract session prefix */
                char session_id[32];
                int prefix_len = flen - slen;
                if (prefix_len <= 0 || prefix_len >= (int)sizeof(session_id)) continue;
                memcpy(session_id, fent->d_name, prefix_len);
                session_id[prefix_len] = '\0';
                /* Read session.json */
                char json_path[600];
                snprintf(json_path, sizeof(json_path), "%s/%s", day_dir, fent->d_name);
                FILE *f = fopen(json_path, "r");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (fsize <= 0 || fsize > 4096) { fclose(f); continue; }
                char *buf = malloc(fsize + 1);
                if (!buf) { fclose(f); continue; }
                fread(buf, 1, fsize, f);
                buf[fsize] = '\0';
                fclose(f);
                cJSON *j = cJSON_Parse(buf);
                free(buf);
                if (!j) continue;
                cJSON *j_start = cJSON_GetObjectItem(j, "start_epoch_ms");
                cJSON *j_end = cJSON_GetObjectItem(j, "end_epoch_ms");
                cJSON *j_drift = cJSON_GetObjectItem(j, "clock_drift_ms");
                if (j_start && cJSON_IsNumber(j_start)) {
                    recreate_session_t *s = &sessions[n_sessions++];
                    strlcpy(s->session_dir, day_dir, sizeof(s->session_dir));
                    strlcpy(s->session_id, session_id, sizeof(s->session_id));
                    s->start_epoch_ms = (int64_t)j_start->valuedouble;
                    s->end_epoch_ms = (j_end && cJSON_IsNumber(j_end)) ? (int64_t)j_end->valuedouble : 0;
                    s->clock_drift_ms = (j_drift && cJSON_IsNumber(j_drift)) ? (int64_t)j_drift->valuedouble : 0;
                }
                cJSON_Delete(j);
            }
            closedir(dd);
        }
        closedir(d);
    }

    ESP_LOGI(TAG, "recreate_edfs_task: found %d sessions", n_sessions);

    /* 3. Sort sessions by start_epoch_ms (simple insertion sort). */
    for (int i = 1; i < n_sessions; i++) {
        recreate_session_t tmp = sessions[i];
        int j = i - 1;
        while (j >= 0 && sessions[j].start_epoch_ms > tmp.start_epoch_ms) {
            sessions[j + 1] = sessions[j];
            j--;
        }
        sessions[j + 1] = tmp;
    }

    /* 4. Generate EDFs for each session in chronological order. */
    for (int i = 0; i < n_sessions; i++) {
        ESP_LOGI(TAG, "recreate_edfs_task: generating EDFs for session %d/%d: %s",
                 i + 1, n_sessions, sessions[i].session_id);
        edf_gen_generate(sessions[i].session_dir, sessions[i].session_id,
                         sessions[i].start_epoch_ms, sessions[i].end_epoch_ms,
                         sessions[i].clock_drift_ms);
    }

    /* 5. Queue unique day folders for upload (not per-session). */
    char queued_days[64][16];
    int n_queued_days = 0;
    for (int i = 0; i < n_sessions; i++) {
        char day_folder[32];
        time_t t = (time_t)(sessions[i].start_epoch_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) {
            t -= 86400;
            localtime_r(&t, &tm);
        }
        snprintf(day_folder, sizeof(day_folder), "%04d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        /* Skip if already queued */
        bool dup = false;
        for (int j = 0; j < n_queued_days; j++) {
            if (strcmp(queued_days[j], day_folder) == 0) { dup = true; break; }
        }
        if (!dup && n_queued_days < 64) {
            strlcpy(queued_days[n_queued_days++], day_folder, sizeof(queued_days[0]));
            uploader_on_day_ready(day_folder);
        }
    }

    ESP_LOGI(TAG, "recreate_edfs_task: done (%d sessions, %d unique days queued)",
             n_sessions, n_queued_days);
    free(sessions);
    vTaskDelete(NULL);
}

/* HTTP handlers for actions. Each responds immediately and runs work in a task. */

static esp_err_t action_reset_state_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "action: reset upload state");
    uploader_reset_state();
    scan_and_queue_uploads();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t action_delete_edfs_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "action: delete all EDF files");
    recursive_delete(SD_SDCARD_DIR);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t action_reset_all_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "action: reset all (state + SDCARD + .sessions)");
    uploader_reset_state();
    recursive_delete(SD_SDCARD_DIR);
    recursive_delete(SD_SESSIONS_DIR);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t action_recreate_edfs_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "action: recreate EDFs");
    /* Run in a background task with PSRAM stack (EDF gen needs 10KB+).
     * Internal-RAM stacks fragment the heap and cause SDMMC DMA allocation
     * failures — see the same pattern in session_writer.c stop_task. */
    const uint32_t stack_size = 16384;
    StackType_t *stack = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!stack || !tcb) {
        free(stack); free(tcb);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        recreate_edfs_task, "recreate_edfs", stack_size, NULL, 5,
        stack, tcb, 1);
    if (!h) {
        free(stack); free(tcb);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task create failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
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
    config.max_uri_handlers = 30;
    config.stack_size = 8192;
    config.max_open_sockets = 7;

    ESP_LOGI(TAG, "starting httpd: stack=%d, handlers=%d, internal free=%u",
             config.stack_size, config.max_uri_handlers,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_err_t herr = httpd_start(&s_httpd, &config);
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd: %s (internal free=%u)",
                 esp_err_to_name(herr),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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

    httpd_uri_t tz_db = { .uri = "/api/tz", .method = HTTP_GET, .handler = tz_get_handler };
    httpd_register_uri_handler(s_httpd, &tz_db);

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

    /* Upload configuration and status endpoints */
    httpd_uri_t up_cfg_get = { .uri = "/api/uploads/config", .method = HTTP_GET, .handler = upload_config_get_handler };
    httpd_uri_t up_cfg_post = { .uri = "/api/uploads/config", .method = HTTP_POST, .handler = upload_config_post_handler };
    httpd_uri_t up_status = { .uri = "/api/uploads/status", .method = HTTP_GET, .handler = upload_status_get_handler };
    httpd_register_uri_handler(s_httpd, &up_cfg_get);
    httpd_register_uri_handler(s_httpd, &up_cfg_post);
    httpd_register_uri_handler(s_httpd, &up_status);

    /* Actions endpoints (Reset State, Delete EDFs, Reset All, Recreate EDFs) */
    httpd_uri_t act_reset_state = { .uri = "/api/actions/reset-state", .method = HTTP_POST, .handler = action_reset_state_handler };
    httpd_uri_t act_delete_edfs = { .uri = "/api/actions/delete-edfs", .method = HTTP_POST, .handler = action_delete_edfs_handler };
    httpd_uri_t act_reset_all = { .uri = "/api/actions/reset-all", .method = HTTP_POST, .handler = action_reset_all_handler };
    httpd_uri_t act_recreate = { .uri = "/api/actions/recreate-edfs", .method = HTTP_POST, .handler = action_recreate_edfs_handler };
    httpd_register_uri_handler(s_httpd, &act_reset_state);
    httpd_register_uri_handler(s_httpd, &act_delete_edfs);
    httpd_register_uri_handler(s_httpd, &act_reset_all);
    httpd_register_uri_handler(s_httpd, &act_recreate);

    if (s_portal_mode) {
        /* Captive-portal probe intercepts (return 302 to trigger portal popup) */
        const char *probes[] = {
            "/hotspot-detect.html",
            "/generate_204",
            "/gen_204",
            "/connecttest.txt",
            "/ncsi.txt",
            "/success.txt",
            "/canonical.html",
            "/service/update2/json",
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
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);
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
            .channel = 1,
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
