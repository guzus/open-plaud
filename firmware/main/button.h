#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* The three top-edge buttons, named by function rather than GPIO so app_main
 * never hardcodes a pin. The physical position -> GPIO map (and the caveat that
 * Left/Middle are unverified) lives in board.h.
 *
 *   BTN_RECORD  Right  — start/stop recording (toggle)
 *   BTN_SYNC    Middle — upload queued clips
 *   BTN_POWER   Left   — screen on/off (and Cancel while recording)
 */
typedef enum {
    BTN_RECORD = 0,
    BTN_SYNC,
    BTN_POWER,
    BTN_COUNT,
} button_id_t;

/* Configure all three buttons as inputs with the internal pull-up enabled
 * (assumed button-to-GND wiring; pressed == BSP_BTN_ACTIVE_LEVEL). */
esp_err_t button_init(void);

/* Instantaneous pressed level only — debouncing/edge-detection is the caller's
 * job (app_main requires consecutive agreeing polls). */
bool button_is_down(button_id_t id);

/* GPIO number behind a button, for boot-time mapping verification logs. */
int button_gpio(button_id_t id);
