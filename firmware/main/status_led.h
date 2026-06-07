#pragma once
#include "esp_err.h"

typedef enum {
    STATUS_LED_BOOT = 0,
    STATUS_LED_SD_READY,
    STATUS_LED_AUDIO_READY,
    STATUS_LED_RECORDING,
    STATUS_LED_DONE,
    STATUS_LED_ERROR,
} status_led_state_t;

esp_err_t status_led_init(void);
void      status_led_set(status_led_state_t state);
void      status_led_deinit(void);
