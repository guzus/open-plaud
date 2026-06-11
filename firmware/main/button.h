#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* PLUS button (BSP_PLUS_BUTTON_GPIO), polled — no interrupts.
 * button_pressed() reports the instantaneous level only; debouncing is the
 * caller's job (app_main requires consecutive agreeing polls). */
esp_err_t button_init(void);
bool      button_pressed(void);
