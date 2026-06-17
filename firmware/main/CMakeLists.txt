idf_component_register(
    SRCS
        "app_main.c"
        "audio.c"
        "button.c"
        "power.c"
        "rec_store.c"
        "sdcard.c"
        "status.c"
        "wav.c"
        "net_upload.c"
        "opus_ogg.c"
        "ble_audio.c"
        "ble_companion.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_driver_gpio esp_driver_i2s esp_driver_i2c esp_driver_sdmmc
        sdmmc fatfs nvs_flash esp_event esp_netif esp_wifi esp_http_client mbedtls esp_codec_dev 78__esp-opus
        esp_lcd esp_adc esp_hw_support bt json
)
