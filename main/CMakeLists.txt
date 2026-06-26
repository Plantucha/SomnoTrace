idf_component_register(SRCS "main.c" "bsp_power.c" "bsp_display.c" "net_provision.c" "as11_ble.c"
                           "sd_storage.c" "session_writer.c" "time_sync.c"
                           "post_therapy.c" "edf_gen.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_wifi esp_netif esp_event nvs_flash esp_http_server json
                                bt mbedtls esp_rom fatfs sdmmc esp-idf-ftpServer)
