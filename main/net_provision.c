/*
 * SomnoTrace - Wi-Fi provisioning, SoftAP captive portal, and NVS config
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#include "net_provision.h"

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
"<title>SomnoTrace Setup</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;display:flex;justify-content:center}"
".card{max-width:420px;width:100%;padding:24px}"
"h1{font-size:1.4rem;margin:.2em 0}p{color:#94a3b8}"
"label{display:block;margin:14px 0 6px;font-weight:600}"
"select,input{width:100%;padding:12px;border-radius:10px;border:1px solid #334155;background:#1e293b;color:#e2e8f0;box-sizing:border-box}"
"button{margin-top:18px;width:100%;padding:13px;border:0;border-radius:10px;background:#22c55e;color:#04210f;font-weight:700;font-size:1rem;cursor:pointer}"
"button.sec{background:#334155;color:#e2e8f0;margin-top:8px}"
"#status{margin-top:14px;min-height:1.2em;color:#fbbf24}"
"</style></head><body><div class=\"card\">"
"<h1>SomnoTrace</h1><p>Select a WiFi network and enter the password.</p>"
"<label for=\"ssid\">Network</label>"
"<select id=\"ssid\"><option value=\"\">-- scan to list networks --</option></select>"
"<button class=\"sec\" onclick=\"scan()\">Scan for networks</button>"
"<label for=\"pass\">Password</label>"
"<input id=\"pass\" type=\"password\" placeholder=\"WiFi password\">"
"<button onclick=\"save()\">Save &amp; Reboot</button>"
"<div id=\"status\"></div></div><script>"
"function scan(){var s=document.getElementById('status');s.textContent='Scanning...';"
"fetch('/scan').then(r=>r.json()).then(d=>{var sel=document.getElementById('ssid');"
"sel.innerHTML='';d.sort((a,b)=>b.rssi-a.rssi);d.forEach(n=>{var o=document.createElement('option');"
"o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.lock?' [SEC]':'');sel.appendChild(o);});"
"s.textContent=d.length+' networks found';}).catch(e=>{s.textContent='Scan failed';});}"
"function save(){var ssid=document.getElementById('ssid').value;var pass=document.getElementById('pass').value;"
"if(!ssid){document.getElementById('status').textContent='Pick a network first';return;}"
"document.getElementById('status').textContent='Saving...';"
"fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})"
".then(r=>r.text()).then(t=>{document.getElementById('status').textContent='Saved. Rebooting...';})"
".catch(e=>{document.getElementById('status').textContent='Save failed';});}"
"window.onload=scan;</script></body></html>";

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
    if (s_portal_mode) {
        httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    } else {
        char page[768];
        snprintf(page, sizeof(page),
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>SomnoTrace</title><style>"
            "body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center}"
            ".c{padding:24px}h1{color:#22c55e}code{font-size:1.3rem;background:#1e293b;padding:6px 12px;border-radius:8px}"
            "</style></head><body><div class=\"c\"><h1>Connected</h1>"
            "<p>This device is reachable at:</p><code>%s</code></div></body></html>",
            s_connected_ip);
        httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    }
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

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "rebooting to apply credentials");
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[256];
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

    char ssid[NETPROV_SSID_MAXLEN + 1] = { 0 };
    char pass[NETPROV_PASS_MAXLEN + 1] = { 0 };
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    form_get(body, "pass", pass, sizeof(pass));

    /* Load existing config, overwrite slot 1, save */
    struct netprov_config cfg;
    netprov_load_config(&cfg);
    strlcpy(cfg.wifi[0].ssid, ssid, sizeof(cfg.wifi[0].ssid));
    strlcpy(cfg.wifi[0].pass, pass, sizeof(cfg.wifi[0].pass));

    if (netprov_save_config(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved credentials for SSID '%s'", ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style=\"font-family:sans-serif\">Saved. Rebooting to connect...</body></html>");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
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
    config.max_uri_handlers = 16;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd");
        return ESP_FAIL;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_httpd, &root);

    if (s_portal_mode) {
        httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
        httpd_register_uri_handler(s_httpd, &scan);
        httpd_register_uri_handler(s_httpd, &save);

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
