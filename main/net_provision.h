/*
 * SomnoTrace - Wi-Fi provisioning, SoftAP captive portal, and NVS config
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define NETPROV_SSID_MAXLEN     32
#define NETPROV_PASS_MAXLEN     64
#define NETPROV_HOSTNAME_MAXLEN 32
#define NETPROV_MAX_SSID_SLOTS  4

/* One stored Wi-Fi credential pair. */
struct netprov_wifi_cred {
    char ssid[NETPROV_SSID_MAXLEN + 1];
    char pass[NETPROV_PASS_MAXLEN + 1];
};

/* Full configuration loaded from NVS. */
struct netprov_config {
    char hostname[NETPROV_HOSTNAME_MAXLEN + 1];
    struct netprov_wifi_cred wifi[NETPROV_MAX_SSID_SLOTS];
};

/* Initialise NVS, netif, event loop and Wi-Fi driver. Call once at boot. */
esp_err_t netprov_init(void);

/* Load the full config from NVS. Returns true if at least one SSID is stored. */
bool netprov_load_config(struct netprov_config *cfg);

/* Save the full config to NVS. */
esp_err_t netprov_save_config(const struct netprov_config *cfg);

/* Try to connect as a station using stored credentials.
 * Scans, picks the strongest matching SSID, tries up to 3 attempts
 * with 5 s spacing per candidate. On success writes IP into ip_out
 * (>= 16 bytes) and returns ESP_OK. */
esp_err_t netprov_try_connect(const struct netprov_config *cfg,
                              char *ip_out, int timeout_ms);

/* Start the SoftAP provisioning portal and captive DNS/HTTP server.
 * SSID is "${hostname}-setup". ap_ip_out (>= 16 bytes) receives the AP IP.
 * After credentials are saved the device reboots. */
esp_err_t netprov_start_portal(const struct netprov_config *cfg, char *ap_ip_out);

/* Start the web server in connected (STA) mode showing the device IP. */
esp_err_t netprov_start_connected_server(const char *ip);

/* Task entry for the captive DNS server (wildcard hijack). 
 * arg is ignored; starts automatically inside netprov_start_portal. */
void netprov_dns_task(void *arg);
