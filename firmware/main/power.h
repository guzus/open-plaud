#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Battery / charge sensing. On the Waveshare board the cell is read through a
 * resistor divider on an ADC pin; the divider ratio is unverified, so the
 * percentage is approximate and calibratable in power.c. */
esp_err_t power_init(void);

/* Battery charge as 0..100, or -1 when unknown (no sensor, ADC unavailable, or
 * an implausible reading — the UI then shows "--"). If `out_charging` is
 * non-NULL it is set true while the charger reports charging. */
int power_battery_pct(bool *out_charging);

/* True while the charger reports charging — a proxy for "USB power present"
 * (this board has no VBUS-sense pin). Used to suppress deep sleep while plugged
 * in so the device stays awake + USB-recognizable. Caveat: a fully-charged
 * battery on USB reads NOT charging, so that edge can still sleep. */
bool power_is_charging(void);

/* True if this boot is a wake from deep sleep (a button press) rather than a
 * cold power-on. Lets the boot path skip the heavy Wi-Fi retry + SD autoformat
 * on every wake. Logs the wake cause. */
bool power_woke_from_button(void);

/* Arm a button EXT1 wake and enter deep sleep. Does NOT return — the chip resets
 * and re-runs app_main when a wake button is pressed. */
void power_deep_sleep(void);
