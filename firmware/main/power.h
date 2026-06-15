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
