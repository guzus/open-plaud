/*
 * PLUS button input for hold-to-record. Deliberately dumb: configure the GPIO
 * as an input with the internal pull-up enabled and report the raw level.
 * app_main owns polling and debouncing, so there are no interrupts to race
 * with and no state to keep here. Pressed polarity is BSP_BTN_ACTIVE_LEVEL in
 * board.h — an assumption to verify on first hardware bring-up.
 */
#include "button.h"
#include "board.h"
#include "driver/gpio.h"

esp_err_t button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_PLUS_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* assumed button-to-GND wiring */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool button_pressed(void)
{
    return gpio_get_level(BSP_PLUS_BUTTON_GPIO) == BSP_BTN_ACTIVE_LEVEL;
}
