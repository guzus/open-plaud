#include "status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#if BSP_HAS_LCD
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif
#include "esp_log.h"

static const char *TAG = "status";

#if BSP_HAS_LCD
#define LCD_HOST SPI2_HOST
#define LCD_ROWS_PER_CHUNK 20
#define LCD_RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define UI_COLOR_BG        LCD_RGB565(9, 11, 13)
#define UI_COLOR_HEADER    LCD_RGB565(25, 28, 32)
#define UI_COLOR_CARD      LCD_RGB565(16, 18, 21)
#define UI_COLOR_TEXT      LCD_RGB565(242, 244, 248)
#define UI_COLOR_MUTED     LCD_RGB565(160, 168, 178)
#define UI_COLOR_BOOT      LCD_RGB565(52, 137, 255)
#define UI_COLOR_READY     LCD_RGB565(43, 214, 142)
#define UI_COLOR_RECORDING LCD_RGB565(255, 64, 88)
#define UI_COLOR_SYNCING   LCD_RGB565(255, 190, 72)
#define UI_COLOR_DONE      LCD_RGB565(68, 206, 230)
#define UI_COLOR_ERROR     LCD_RGB565(255, 82, 180)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_tx_done;
static bool s_lcd_ready;
#endif

#if BSP_HAS_LCD
typedef struct {
    const char *title;
    const char *detail;
    const char *footer;
    uint16_t accent;
} status_screen_t;

typedef struct {
    char ch;
    uint8_t rows[7];
} font_glyph_t;

static const font_glyph_t FONT5X7[] = {
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E } },
    { '6', { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
};

static uint16_t status_color(open_plaud_status_t status)
{
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT: return UI_COLOR_BOOT;
    case OPEN_PLAUD_STATUS_READY: return UI_COLOR_READY;
    case OPEN_PLAUD_STATUS_RECORDING: return UI_COLOR_RECORDING;
    case OPEN_PLAUD_STATUS_SYNCING: return UI_COLOR_SYNCING;
    case OPEN_PLAUD_STATUS_DONE: return UI_COLOR_DONE;
    case OPEN_PLAUD_STATUS_ERROR: return UI_COLOR_ERROR;
    }
    return 0xFFFF;
}

static bool lcd_color_trans_done_cb(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx
)
{
    (void)panel_io;
    (void)edata;

    BaseType_t high_task_woken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (sem != NULL) {
        xSemaphoreGiveFromISR(sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static status_screen_t status_screen(open_plaud_status_t status)
{
    const uint16_t accent = status_color(status);
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT:
        return (status_screen_t){ "BOOT", "STARTING", "MOUNT SD AND AUDIO", accent };
    case OPEN_PLAUD_STATUS_READY:
        return (status_screen_t){ "READY", "HOLD PLUS", "RECORD TO MICROSD", accent };
    case OPEN_PLAUD_STATUS_RECORDING:
        return (status_screen_t){ "REC", "RELEASE TO STOP", "CAPTURING WAV", accent };
    case OPEN_PLAUD_STATUS_SYNCING:
        return (status_screen_t){ "SYNCING", "UPLOADING", "WAIT FOR SERVER", accent };
    case OPEN_PLAUD_STATUS_DONE:
        return (status_screen_t){ "DONE", "SAVED", "CLIP SENT", accent };
    case OPEN_PLAUD_STATUS_ERROR:
        return (status_screen_t){ "ERROR", "CHECK SD WIFI", "SEE SERIAL LOG", accent };
    }
    return (status_screen_t){ "STATUS", "UNKNOWN", "SEE SERIAL LOG", accent };
}

static int text_width_px(const char *text, int scale)
{
    if (text == NULL || scale <= 0) {
        return 0;
    }
    size_t len = strlen(text);
    if (len == 0) {
        return 0;
    }
    return (int)((len * 5 + (len - 1)) * (size_t)scale);
}

static bool lcd_draw_bitmap_sync(int x1, int y1, int x2, int y2, const uint16_t *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, pixels);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD draw failed: %s", esp_err_to_name(err));
        return false;
    }
    if (s_lcd_tx_done != NULL &&
        xSemaphoreTake(s_lcd_tx_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD draw timed out waiting for DMA completion");
        return false;
    }
    return true;
}

static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_lcd_ready || s_panel == NULL || w <= 0 || h <= 0) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > BSP_LCD_H_RES) {
        w = BSP_LCD_H_RES - x;
    }
    if (y + h > BSP_LCD_V_RES) {
        h = BSP_LCD_V_RES - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    static uint16_t pixels[BSP_LCD_H_RES * LCD_ROWS_PER_CHUNK];
    int drawn = 0;
    while (drawn < h) {
        const int rows = (h - drawn > LCD_ROWS_PER_CHUNK)
            ? LCD_ROWS_PER_CHUNK
            : h - drawn;
        for (int i = 0; i < w * rows; i++) {
            pixels[i] = color;
        }
        if (!lcd_draw_bitmap_sync(x, y + drawn, x + w, y + drawn + rows, pixels)) {
            return;
        }
        drawn += rows;
    }
}

static void lcd_fill_screen(uint16_t color)
{
    lcd_fill_rect(0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, color);
}

static bool font5x7_rows(char ch, uint8_t rows[7])
{
    memset(rows, 0, 7);
    if (ch == ' ') {
        return true;
    }

    for (size_t i = 0; i < sizeof(FONT5X7) / sizeof(FONT5X7[0]); i++) {
        if (FONT5X7[i].ch == ch) {
            memcpy(rows, FONT5X7[i].rows, 7);
            return true;
        }
    }
    return false;
}

static void lcd_draw_text(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg)
{
    const int w = text_width_px(text, scale);
    const int h = 7 * scale;
    if (text == NULL || scale <= 0 || w <= 0 || h <= 0 ||
        w > BSP_LCD_H_RES || h > 32) {
        return;
    }

    static uint16_t pixels[BSP_LCD_H_RES * 32];
    for (int i = 0; i < w * h; i++) {
        pixels[i] = bg;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        uint8_t glyph[7];
        if (!font5x7_rows(text[i], glyph)) {
            continue;
        }
        const int glyph_x = (int)i * 6 * scale;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if ((glyph[row] & (1 << (4 - col))) == 0) {
                    continue;
                }
                for (int py = 0; py < scale; py++) {
                    for (int px = 0; px < scale; px++) {
                        const int dst_x = glyph_x + col * scale + px;
                        const int dst_y = row * scale + py;
                        pixels[dst_y * w + dst_x] = fg;
                    }
                }
            }
        }
    }
    lcd_draw_bitmap_sync(x, y, x + w, y + h, pixels);
}

static void lcd_draw_text_centered(
    int y,
    const char *text,
    int scale,
    uint16_t fg,
    uint16_t bg
)
{
    const int x = (BSP_LCD_H_RES - text_width_px(text, scale)) / 2;
    lcd_draw_text(x, y, text, scale, fg, bg);
}

static void lcd_draw_status(open_plaud_status_t status)
{
    const status_screen_t screen = status_screen(status);
    lcd_fill_screen(UI_COLOR_BG);
    lcd_fill_rect(0, 0, BSP_LCD_H_RES, 34, UI_COLOR_HEADER);
    lcd_draw_text_centered(10, "OPEN PLAUD", 2, UI_COLOR_TEXT, UI_COLOR_HEADER);

    lcd_fill_rect(16, 50, BSP_LCD_H_RES - 32, 124, UI_COLOR_CARD);
    lcd_fill_rect(16, 50, BSP_LCD_H_RES - 32, 5, screen.accent);
    lcd_draw_text_centered(76, screen.title, 4, screen.accent, UI_COLOR_CARD);
    lcd_draw_text_centered(126, screen.detail, 2, UI_COLOR_TEXT, UI_COLOR_CARD);
    lcd_draw_text_centered(154, screen.footer, 1, UI_COLOR_MUTED, UI_COLOR_CARD);

    lcd_fill_rect(24, 194, BSP_LCD_H_RES - 48, 12, UI_COLOR_HEADER);
    lcd_fill_rect(24, 194, BSP_LCD_H_RES - 48, 12, screen.accent);
    lcd_fill_rect(0, 224, BSP_LCD_H_RES, 16, screen.accent);
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
    s_lcd_tx_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lcd_tx_done != NULL, ESP_ERR_NO_MEM, TAG, "lcd sem");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BSP_LCD_DC_GPIO,
        .cs_gpio_num = BSP_LCD_CS_GPIO,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_trans_done_cb,
        .user_ctx = s_lcd_tx_done,
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
    lcd_draw_status(status);
#elif BSP_HAS_RGB_LED
    rgb_show(status);
#else
    (void)status;
#endif
}
