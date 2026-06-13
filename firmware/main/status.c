#include "status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#if BSP_HAS_LCD
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#endif
#include "esp_log.h"

static const char *TAG = "status";

#if BSP_HAS_LCD
#define LCD_HOST SPI2_HOST
#define LCD_ROWS_PER_CHUNK 20

static esp_lcd_panel_handle_t s_panel;
static bool s_lcd_ready;
#endif

#if BSP_HAS_LCD
static uint16_t status_color(open_plaud_status_t status)
{
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT: return 0x001F;      /* blue */
    case OPEN_PLAUD_STATUS_READY: return 0x07E0;     /* green */
    case OPEN_PLAUD_STATUS_RECORDING: return 0xF800; /* red */
    case OPEN_PLAUD_STATUS_SYNCING: return 0xFFE0;   /* yellow */
    case OPEN_PLAUD_STATUS_DONE: return 0x07FF;      /* cyan */
    case OPEN_PLAUD_STATUS_ERROR: return 0xF81F;     /* magenta */
    }
    return 0xFFFF;
}
#endif

static const char *status_name(open_plaud_status_t status)
{
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT: return "boot";
    case OPEN_PLAUD_STATUS_READY: return "ready";
    case OPEN_PLAUD_STATUS_RECORDING: return "recording";
    case OPEN_PLAUD_STATUS_SYNCING: return "syncing";
    case OPEN_PLAUD_STATUS_DONE: return "done";
    case OPEN_PLAUD_STATUS_ERROR: return "error";
    }
    return "unknown";
}

#if BSP_HAS_RGB_LED
static void rgb_set(bool red, bool green, bool blue)
{
    const int on = BSP_RGB_LED_ACTIVE_LEVEL;
    const int off = !BSP_RGB_LED_ACTIVE_LEVEL;
    gpio_set_level(BSP_RGB_LED_R_GPIO, red ? on : off);
    gpio_set_level(BSP_RGB_LED_G_GPIO, green ? on : off);
    gpio_set_level(BSP_RGB_LED_B_GPIO, blue ? on : off);
}

static void rgb_show(open_plaud_status_t status)
{
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT: rgb_set(false, false, true); return;
    case OPEN_PLAUD_STATUS_READY: rgb_set(false, true, false); return;
    case OPEN_PLAUD_STATUS_RECORDING: rgb_set(true, false, false); return;
    case OPEN_PLAUD_STATUS_SYNCING: rgb_set(true, true, false); return;
    case OPEN_PLAUD_STATUS_DONE: rgb_set(false, true, true); return;
    case OPEN_PLAUD_STATUS_ERROR: rgb_set(true, false, true); return;
    }
}
#endif

esp_err_t status_init(void)
{
#if BSP_HAS_LCD
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BSP_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(BSP_LCD_BL_GPIO, 0);

    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SCLK_GPIO,
        .mosi_io_num = BSP_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * LCD_ROWS_PER_CHUNK * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "LCD SPI init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BSP_LCD_DC_GPIO,
        .cs_gpio_num = BSP_LCD_CS_GPIO,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io),
        TAG,
        "panel_io"
    );

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "lcd_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "lcd_init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "lcd_invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "lcd_on");

    gpio_set_level(BSP_LCD_BL_GPIO, 1);
    s_lcd_ready = true;
    return ESP_OK;
#elif BSP_HAS_RGB_LED
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_RGB_LED_R_GPIO)
            | (1ULL << BSP_RGB_LED_G_GPIO)
            | (1ULL << BSP_RGB_LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "rgb gpio");
    rgb_set(false, false, false);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

void status_show(open_plaud_status_t status)
{
    ESP_LOGI(TAG, "status: %s", status_name(status));
#if BSP_HAS_LCD
    if (!s_lcd_ready || s_panel == NULL) {
        return;
    }

    static uint16_t pixels[BSP_LCD_H_RES * LCD_ROWS_PER_CHUNK];
    const uint16_t color = status_color(status);
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        pixels[i] = color;
    }
    for (int y = 0; y < BSP_LCD_V_RES; y += LCD_ROWS_PER_CHUNK) {
        const int y2 = (y + LCD_ROWS_PER_CHUNK > BSP_LCD_V_RES)
            ? BSP_LCD_V_RES
            : y + LCD_ROWS_PER_CHUNK;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H_RES, y2, pixels);
    }
#elif BSP_HAS_RGB_LED
    rgb_show(status);
#else
    (void)status;
#endif
}
