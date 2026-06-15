/*
 * Battery percentage and charge status for the status UI.
 *
 * The Waveshare board ties the cell to an ADC pin (BSP_BAT_ADC_GPIO) through a
 * resistor divider gated by BSP_BAT_EN_GPIO, with a charge-status line on
 * BSP_CHG_STAT_GPIO. ADC1 is used on purpose: ADC2 is shared with Wi-Fi and
 * returns errors while the radio is up (i.e. during upload).
 *
 * CALIBRATED on hardware (2026-06-16): the divider is 3:1 (BAT_DIVIDER 3.0) —
 * the ADC reads ~1374 mV for a ~4.12 V cell on USB. EN (active-high) and the
 * charge-stat pin behave as assumed. The percent map (3300 mV = 0%, 4200 mV =
 * 100%) is still a coarse LiPo approximation; an implausible reading degrades to
 * "unknown" (-1) rather than a wrong number.
 *
 * Boards without an ADC battery pin (e.g. the custom Rev A, which has an I2C
 * fuel gauge) compile the stub and always report "unknown".
 */
#include "power.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "power";

#if defined(BSP_BAT_ADC_GPIO)
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ---- Calibratable constants (see UNVERIFIED note above) ----------------- */
#define BAT_DIVIDER          3.0f   /* Vbat = Vadc * BAT_DIVIDER; 3:1 divider, */
                                    /* calibrated on HW 2026-06-16 (adc 1374mV -> 4.12V on USB) */
#define BAT_EN_ACTIVE_LEVEL  1      /* drive EN to this to connect divider   */
#define BAT_FULL_MV          4200   /* LiPo full                             */
#define BAT_EMPTY_MV         3300   /* LiPo empty (under light load)         */
#define BAT_PLAUSIBLE_MIN_MV 2500   /* below this: divider off / no battery  */
#define BAT_PLAUSIBLE_MAX_MV 4400   /* above this: bad reading               */
#define BAT_SETTLE_MS        10
#define BAT_SAMPLES          8

/* GPIO1 == ADC1 channel 0 on the ESP32-S3. */
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_0
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

esp_err_t power_init(void)
{
    /* EN as output (parked inactive — only enabled around a read), charge-stat
     * as input with a pull-up (open-drain LOW == charging on most chargers). */
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << BSP_BAT_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&en);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bat-en gpio config failed: %s", esp_err_to_name(err));
    }
    gpio_set_level(BSP_BAT_EN_GPIO, !BAT_EN_ACTIVE_LEVEL);

    gpio_config_t chg = {
        .pin_bit_mask = 1ULL << BSP_CHG_STAT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&chg);

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed: %s — battery shows '--'", esp_err_to_name(err));
        return ESP_OK;   /* non-fatal: UI just hides the battery */
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc channel config failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable: %s — battery shows '--'",
                 esp_err_to_name(err));
        s_cali = NULL;
        return ESP_OK;
    }

    s_ready = true;
    bool charging = false;
    int pct = power_battery_pct(&charging);
    ESP_LOGI(TAG, "battery sense ready (GPIO%d, divider %.1f): %d%%%s",
             BSP_BAT_ADC_GPIO, (double)BAT_DIVIDER, pct, charging ? " charging" : "");
    return ESP_OK;
}

int power_battery_pct(bool *out_charging)
{
    if (out_charging) {
        *out_charging = (gpio_get_level(BSP_CHG_STAT_GPIO) == 0);
    }
    if (!s_ready || s_cali == NULL) {
        return -1;
    }

    gpio_set_level(BSP_BAT_EN_GPIO, BAT_EN_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(BAT_SETTLE_MS));

    int acc = 0, ok = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            acc += raw;
            ok++;
        }
    }
    gpio_set_level(BSP_BAT_EN_GPIO, !BAT_EN_ACTIVE_LEVEL);
    if (ok == 0) {
        return -1;
    }

    int raw_avg = acc / ok;
    int adc_mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw_avg, &adc_mv) != ESP_OK) {
        return -1;
    }
    int vbat_mv = (int)((float)adc_mv * BAT_DIVIDER);
    ESP_LOGD(TAG, "battery raw=%d adc=%dmV vbat=%dmV", raw_avg, adc_mv, vbat_mv);

    if (vbat_mv < BAT_PLAUSIBLE_MIN_MV || vbat_mv > BAT_PLAUSIBLE_MAX_MV) {
        return -1;   /* divider likely off or pin not wired as assumed */
    }

    int pct = (vbat_mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

#else  /* no ADC battery pin on this board */

esp_err_t power_init(void)
{
    ESP_LOGI(TAG, "no ADC battery pin on %s — battery shows '--'", BSP_BOARD_NAME);
    return ESP_OK;
}

int power_battery_pct(bool *out_charging)
{
    if (out_charging) {
        *out_charging = false;
    }
    return -1;
}

#endif
