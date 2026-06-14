idf_component_register(
    SRCS
        "app_main.c"
        "audio.c"
        "button.c"
        "rec_store.c"
        "sdcard.c"
        "status.c"
        "wav.c"
        "net_upload.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_driver_gpio esp_driver_i2s esp_driver_i2c esp_driver_sdmmc
        sdmmc fatfs nvs_flash esp_event esp_netif esp_wifi esp_http_client mbedtls esp_codec_dev
        esp_lcd
)
