#include "status_led.h"

#include <stdint.h>

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static void fill(rgb_t color)
{
    if (!s_strip) return;
    for (int i = 0; i < BSP_WS2812_COUNT; ++i) {
        led_strip_set_pixel(s_strip, i, color.r, color.g, color.b);
    }
    led_strip_refresh(s_strip);
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_WS2812_GPIO,
        .max_leds = BSP_WS2812_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
                        TAG, "led_strip_new_rmt_device");
    led_strip_clear(s_strip);
    status_led_set(STATUS_LED_BOOT);
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_BOOT:
        fill((rgb_t){ .r = 8, .g = 8, .b = 8 });
        break;
    case STATUS_LED_SD_READY:
        fill((rgb_t){ .r = 0, .g = 0, .b = 24 });
        break;
    case STATUS_LED_AUDIO_READY:
        fill((rgb_t){ .r = 0, .g = 20, .b = 20 });
        break;
    case STATUS_LED_RECORDING:
        fill((rgb_t){ .r = 32, .g = 0, .b = 0 });
        break;
    case STATUS_LED_DONE:
        fill((rgb_t){ .r = 0, .g = 24, .b = 0 });
        break;
    case STATUS_LED_ERROR:
    default:
        fill((rgb_t){ .r = 32, .g = 4, .b = 0 });
        break;
    }
}

void status_led_deinit(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
}
