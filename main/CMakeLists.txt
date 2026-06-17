idf_component_register(SRCS "main.c" "bsp_power.c" "bsp_display.c" "net_provision.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_wifi esp_netif esp_event nvs_flash esp_http_server json)
