idf_component_register(
    SRCS "uploader.c" "uploader_state.c" "uploader_smb.c" "uploader_sleephq.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash json esp_http_client mbedtls libsmb2 esp_timer
)
