/*
 * Three top-edge buttons for the recorder UX. Deliberately dumb: each GPIO is
 * an input with the internal pull-up enabled and we report the raw level.
 * app_main owns polling, debouncing, and edge detection, so there are no
 * interrupts to race with and no state to keep here. Pressed polarity is
 * BSP_BTN_ACTIVE_LEVEL; the physical position -> GPIO map is in board.h and is
 * unverified for Left/Middle — boot logs the mapping so it can be checked.
 */
#include "button.h"
#include "board.h"
#include "driver/gpio.h"

static const int s_gpio[BTN_COUNT] = {
    [BTN_RECORD] = BSP_BTN_RIGHT_GPIO,
    [BTN_SYNC]   = BSP_BTN_MIDDLE_GPIO,
    [BTN_POWER]  = BSP_BTN_LEFT_GPIO,
};

esp_err_t button_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) {
        mask |= 1ULL << s_gpio[i];
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* assumed button-to-GND wiring */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool button_is_down(button_id_t id)
{
    if (id < 0 || id >= BTN_COUNT) {
        return false;
    }
    return gpio_get_level(s_gpio[id]) == BSP_BTN_ACTIVE_LEVEL;
}

int button_gpio(button_id_t id)
{
    if (id < 0 || id >= BTN_COUNT) {
        return -1;
    }
    return s_gpio[id];
}
