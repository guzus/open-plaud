idf_component_register(
    SRCS
        "app_main.c"
        "audio.c"
        "sdcard.c"
        "wav.c"
        "net_upload.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_driver_i2s esp_driver_i2c esp_driver_sdmmc
        sdmmc fatfs nvs_flash esp_wifi esp_http_client esp_codec_dev
)
