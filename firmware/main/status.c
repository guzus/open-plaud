#include "status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
#include "power.h"
#include "sdcard.h"
#include "sdkconfig.h"

static const char *TAG = "status";

/* Shared across LCD + RGB builds so the public API has one source of truth. */
static open_plaud_status_t s_status = OPEN_PLAUD_STATUS_BOOT;
static status_info_t s_info = { -1, -1, -1, -1, -1, NULL };
static char s_clip[20];   /* owns the clip name; callers' pointers may dangle */

#if BSP_HAS_LCD
#define LCD_HOST SPI2_HOST
#define LCD_ROWS_PER_CHUNK 20
#define LCD_RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define UI_COLOR_BG        LCD_RGB565(9, 11, 13)
#define UI_COLOR_HEADER    LCD_RGB565(25, 28, 32)
#define UI_COLOR_CARD      LCD_RGB565(16, 18, 21)
#define UI_COLOR_TEXT      LCD_RGB565(242, 244, 248)
#define UI_COLOR_MUTED     LCD_RGB565(120, 128, 138)
#define UI_COLOR_DIM       LCD_RGB565(64, 70, 78)
#define UI_COLOR_WORDMARK  LCD_RGB565(43, 214, 142)
#define UI_COLOR_BOOT      LCD_RGB565(52, 137, 255)
#define UI_COLOR_READY     LCD_RGB565(43, 214, 142)
#define UI_COLOR_RECORDING LCD_RGB565(255, 64, 88)
#define UI_COLOR_SYNCING   LCD_RGB565(90, 176, 255)
#define UI_COLOR_DONE      LCD_RGB565(68, 206, 230)
#define UI_COLOR_SAVED     LCD_RGB565(43, 214, 142)
#define UI_COLOR_OFFLINE   LCD_RGB565(255, 183, 77)
#define UI_COLOR_ERROR     LCD_RGB565(255, 82, 180)
#define UI_COLOR_REC_HINT  LCD_RGB565(255, 79, 163)

/* Layout regions (240x240). Partial redraws target the TIMER and FOOTER bands. */
#define UI_HINT_Y     0
#define UI_HINT_H     28
#define UI_CARD_X     10
#define UI_CARD_Y     36
#define UI_CARD_W     (BSP_LCD_H_RES - 20)
#define UI_CARD_H     166
#define UI_TITLE_Y    46
#define UI_TIMER_Y    92
#define UI_DETAIL_Y   98
#define UI_SUB_Y      140
#define UI_BAR_X      (UI_CARD_X + 16)
#define UI_BAR_W      (UI_CARD_W - 32)
#define UI_BAR_Y      170
#define UI_BAR_H      10
#define UI_FOOTER_Y   208
#define UI_FOOTER_H   30

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_tx_done;
static bool s_lcd_ready;
static bool s_display_on = true;

typedef struct {
    char ch;
    uint8_t rows[7];
} font_glyph_t;

static const font_glyph_t FONT5X7[] = {
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
    { '/', { 0x01, 0x02, 0x04, 0x04, 0x08, 0x10, 0x10 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
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
    case OPEN_PLAUD_STATUS_SAVED: return UI_COLOR_SAVED;
    case OPEN_PLAUD_STATUS_SYNCING: return UI_COLOR_SYNCING;
    case OPEN_PLAUD_STATUS_DONE: return UI_COLOR_DONE;
    case OPEN_PLAUD_STATUS_OFFLINE: return UI_COLOR_OFFLINE;
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
        w > BSP_LCD_H_RES || h > 40) {
        return;
    }

    static uint16_t pixels[BSP_LCD_H_RES * 40];
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

static void lcd_draw_text_centered(int y, const char *text, int scale, uint16_t fg, uint16_t bg)
{
    const int x = (BSP_LCD_H_RES - text_width_px(text, scale)) / 2;
    lcd_draw_text(x, y, text, scale, fg, bg);
}

/* ---- button hint row -------------------------------------------------------
 * Three columns (Left / Middle / Right), aligned under the three top-edge
 * buttons. Labels are honest: a control that can't be acted on right now (e.g.
 * Cancel mid-sync, while the upload call blocks) is shown dimmed, never as a
 * live action. */
static void draw_hint(int col, const char *txt, uint16_t fg)
{
    const int colw = BSP_LCD_H_RES / 3;
    const int x = col * colw + (colw - text_width_px(txt, 2)) / 2;
    lcd_draw_text(x, UI_HINT_Y + 7, txt, 2, fg, UI_COLOR_HEADER);
}

static void draw_hints(open_plaud_status_t status, const status_info_t *info)
{
    lcd_fill_rect(0, UI_HINT_Y, BSP_LCD_H_RES, UI_HINT_H, UI_COLOR_HEADER);
    const bool has_queue = info && info->pending > 0;
    const uint16_t sync_col = has_queue ? UI_COLOR_SYNCING : UI_COLOR_DIM;

    /* Columns: 0 = Left (power), 1 = Middle (sync), 2 = Right (record). */
    switch (status) {
    case OPEN_PLAUD_STATUS_RECORDING:
        draw_hint(0, "CANCEL", UI_COLOR_RECORDING);
        draw_hint(1, "-", UI_COLOR_DIM);
        draw_hint(2, "STOP", UI_COLOR_RECORDING);
        break;
    case OPEN_PLAUD_STATUS_SYNCING:
        draw_hint(0, "-", UI_COLOR_DIM);
        draw_hint(1, "BUSY", UI_COLOR_MUTED);
        draw_hint(2, "-", UI_COLOR_DIM);
        break;
    case OPEN_PLAUD_STATUS_OFFLINE:
        draw_hint(0, "PWR", UI_COLOR_MUTED);
        draw_hint(1, "RETRY", UI_COLOR_SYNCING);
        draw_hint(2, "REC", UI_COLOR_REC_HINT);
        break;
    case OPEN_PLAUD_STATUS_BOOT:
    case OPEN_PLAUD_STATUS_ERROR:
        draw_hint(0, "-", UI_COLOR_DIM);
        draw_hint(1, "-", UI_COLOR_DIM);
        draw_hint(2, "-", UI_COLOR_DIM);
        break;
    default: /* READY / SAVED / DONE */
        draw_hint(0, "PWR", UI_COLOR_MUTED);
        draw_hint(1, "SYNC", sync_col);
        draw_hint(2, "REC", UI_COLOR_REC_HINT);
        break;
    }
}

/* ---- battery + storage footer --------------------------------------------- */
static void draw_footer(void)
{
    lcd_fill_rect(0, UI_FOOTER_Y, BSP_LCD_H_RES, UI_FOOTER_H, UI_COLOR_BG);

    char bat[24];
    bool charging = false;
    int pct = power_battery_pct(&charging);
    if (pct < 0) {
        snprintf(bat, sizeof bat, "BAT --");
    } else {
        snprintf(bat, sizeof bat, "BAT %d%%%s", pct, charging ? "+" : "");
    }

    char sd[32];
    uint64_t used = 0, total = 0;
    if (sdcard_usage(&used, &total) == ESP_OK && total > 0) {
        unsigned used_tenths = (unsigned)(used / 100000000ULL);          /* 0.1 GB */
        snprintf(sd, sizeof sd, "SD %u.%uG", used_tenths / 10, used_tenths % 10);
    } else {
        snprintf(sd, sizeof sd, "SD --");
    }

    char foot[64];
    snprintf(foot, sizeof foot, "%s  %s", bat, sd);
    lcd_draw_text_centered(UI_FOOTER_Y + 7, foot, 2, UI_COLOR_MUTED, UI_COLOR_BG);
}

static void draw_bar(int filled_pct, uint16_t color)
{
    if (filled_pct < 0) {
        filled_pct = 0;
    }
    if (filled_pct > 100) {
        filled_pct = 100;
    }
    lcd_fill_rect(UI_BAR_X, UI_BAR_Y, UI_BAR_W, UI_BAR_H, UI_COLOR_HEADER);
    const int fw = UI_BAR_W * filled_pct / 100;
    if (fw > 0) {
        lcd_fill_rect(UI_BAR_X, UI_BAR_Y, fw, UI_BAR_H, color);
    }
}

static void fmt_mmss(int seconds, char *out, size_t n)
{
    if (seconds < 0) {
        seconds = 0;
    }
    snprintf(out, n, "%02d:%02d", seconds / 60, seconds % 60);
}

/* Repaint just the recording timer (and elapsed bar) — the per-second region
 * touched from inside the capture loop. Small bitmaps only; no full redraw. */
static void draw_timer(int elapsed_s)
{
    char t[8];
    fmt_mmss(elapsed_s, t, sizeof t);
    lcd_fill_rect(UI_CARD_X, UI_TIMER_Y, UI_CARD_W, 7 * 5, UI_COLOR_CARD);
    lcd_draw_text_centered(UI_TIMER_Y, t, 5, UI_COLOR_TEXT, UI_COLOR_CARD);

    int pct = 0;
    if (CONFIG_OPEN_PLAUD_MAX_REC_SECONDS > 0) {
        pct = elapsed_s * 100 / CONFIG_OPEN_PLAUD_MAX_REC_SECONDS;
    }
    draw_bar(pct, UI_COLOR_RECORDING);
}

static void lcd_draw_full(open_plaud_status_t status, const status_info_t *info)
{
    const uint16_t accent = status_color(status);
    lcd_fill_rect(0, UI_HINT_H, BSP_LCD_H_RES, BSP_LCD_V_RES - UI_HINT_H, UI_COLOR_BG);
    draw_hints(status, info);

    lcd_fill_rect(UI_CARD_X, UI_CARD_Y, UI_CARD_W, UI_CARD_H, UI_COLOR_CARD);
    lcd_fill_rect(UI_CARD_X, UI_CARD_Y, UI_CARD_W, 4, accent);

    char detail[48];
    detail[0] = '\0';

    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT:
        lcd_draw_text_centered(UI_TITLE_Y, "BOOT", 5, accent, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_DETAIL_Y, "STARTING", 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        break;
    case OPEN_PLAUD_STATUS_READY:
        lcd_draw_text_centered(UI_TITLE_Y, "READY", 5, accent, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_DETAIL_Y, "TAP REC", 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_SUB_Y, "TAP RIGHT TO REC", 2, UI_COLOR_MUTED, UI_COLOR_CARD);
        break;
    case OPEN_PLAUD_STATUS_RECORDING:
        lcd_draw_text_centered(UI_TITLE_Y, "REC", 5, accent, UI_COLOR_CARD);
        draw_timer(info ? info->elapsed_s : 0);
        if (info && info->clip) {
            lcd_draw_text_centered(UI_SUB_Y, info->clip, 2, UI_COLOR_MUTED, UI_COLOR_CARD);
        }
        break;
    case OPEN_PLAUD_STATUS_SAVED:
        lcd_draw_text_centered(UI_TITLE_Y, "SAVED", 5, accent, UI_COLOR_CARD);
        if (info && info->pending > 0) {
            snprintf(detail, sizeof detail, "%d TO SYNC", info->pending);
        } else {
            snprintf(detail, sizeof detail, "QUEUED");
        }
        lcd_draw_text_centered(UI_DETAIL_Y, detail, 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_SUB_Y, "TAP SYNC TO UPLOAD", 2, UI_COLOR_MUTED, UI_COLOR_CARD);
        break;
    case OPEN_PLAUD_STATUS_SYNCING:
        lcd_draw_text_centered(UI_TITLE_Y, "SYNCING", 5, accent, UI_COLOR_CARD);
        if (info && info->sync_total > 0) {
            const int done = info->sync_done < 0 ? 0 : info->sync_done;
            const int active = info->upload_pct < 0 ? 0 : info->upload_pct;
            if (info->upload_pct >= 0 && done < info->sync_total) {
                snprintf(detail, sizeof detail, "%d/%d %d%%",
                         done, info->sync_total, active);
            } else {
                snprintf(detail, sizeof detail, "%d OF %d", done, info->sync_total);
            }
            lcd_draw_text_centered(UI_DETAIL_Y, detail, 3, UI_COLOR_TEXT, UI_COLOR_CARD);
            int pct = (done * 100 + active) / info->sync_total;
            if (pct > 100) pct = 100;
            draw_bar(pct, accent);
        } else {
            lcd_draw_text_centered(UI_DETAIL_Y, "UPLOADING", 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        }
        if (info && info->clip) {
            lcd_draw_text_centered(UI_SUB_Y, info->clip, 2, UI_COLOR_MUTED, UI_COLOR_CARD);
        }
        break;
    case OPEN_PLAUD_STATUS_DONE:
        lcd_draw_text_centered(UI_TITLE_Y, "DONE", 5, accent, UI_COLOR_CARD);
        if (info && info->sync_done > 0) {
            snprintf(detail, sizeof detail, "%d SENT", info->sync_done);
        } else {
            snprintf(detail, sizeof detail, "ALL SYNCED");
        }
        lcd_draw_text_centered(UI_DETAIL_Y, detail, 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        break;
    case OPEN_PLAUD_STATUS_OFFLINE:
        lcd_draw_text_centered(UI_TITLE_Y, "NO WIFI", 5, accent, UI_COLOR_CARD);
        if (info && info->pending > 0) {
            snprintf(detail, sizeof detail, "%d QUEUED", info->pending);
        } else {
            snprintf(detail, sizeof detail, "QUEUED");
        }
        lcd_draw_text_centered(UI_DETAIL_Y, detail, 3, UI_COLOR_TEXT, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_SUB_Y, "SYNC POSTPONED", 2, UI_COLOR_OFFLINE, UI_COLOR_CARD);
        break;
    case OPEN_PLAUD_STATUS_ERROR:
        lcd_draw_text_centered(UI_TITLE_Y, "ERROR", 5, accent, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_DETAIL_Y, "CHECK SD WIFI", 2, UI_COLOR_TEXT, UI_COLOR_CARD);
        lcd_draw_text_centered(UI_SUB_Y, "SEE SERIAL LOG", 2, UI_COLOR_MUTED, UI_COLOR_CARD);
        break;
    }

    draw_footer();
}
#endif /* BSP_HAS_LCD */

static const char *status_name(open_plaud_status_t status)
{
    switch (status) {
    case OPEN_PLAUD_STATUS_BOOT: return "boot";
    case OPEN_PLAUD_STATUS_READY: return "ready";
    case OPEN_PLAUD_STATUS_RECORDING: return "recording";
    case OPEN_PLAUD_STATUS_SAVED: return "saved";
    case OPEN_PLAUD_STATUS_SYNCING: return "syncing";
    case OPEN_PLAUD_STATUS_DONE: return "done";
    case OPEN_PLAUD_STATUS_OFFLINE: return "offline";
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
    case OPEN_PLAUD_STATUS_SAVED: rgb_set(false, true, false); return;
    case OPEN_PLAUD_STATUS_SYNCING: rgb_set(true, true, false); return;
    case OPEN_PLAUD_STATUS_DONE: rgb_set(false, true, true); return;
    case OPEN_PLAUD_STATUS_OFFLINE: rgb_set(true, true, false); return;
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
    s_display_on = true;
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

void status_update(open_plaud_status_t status, const status_info_t *info)
{
    s_status = status;
    if (info) {
        s_info = *info;
        /* Copy the clip name into our own storage: callers pass a pointer into a
         * stack buffer that dangles once they return. */
        if (info->clip) {
            snprintf(s_clip, sizeof s_clip, "%s", info->clip);
            s_info.clip = s_clip;
        } else {
            s_info.clip = NULL;
        }
    } else {
        s_info = (status_info_t){ -1, -1, -1, -1, -1, NULL };
    }
    ESP_LOGI(TAG, "status: %s", status_name(status));
#if BSP_HAS_LCD
    if (!s_lcd_ready || s_panel == NULL || !s_display_on) {
        return;
    }
    lcd_draw_full(status, &s_info);
#elif BSP_HAS_RGB_LED
    rgb_show(status);
#endif
}

void status_show(open_plaud_status_t status)
{
    status_update(status, NULL);
}

void status_tick_recording(int elapsed_s)
{
    s_info.elapsed_s = elapsed_s;
    if (s_status != OPEN_PLAUD_STATUS_RECORDING) {
        return;
    }
#if BSP_HAS_LCD
    if (!s_lcd_ready || s_panel == NULL || !s_display_on) {
        return;
    }
    /* Timer only. The footer (battery/storage) is NOT refreshed here on purpose:
     * this runs on the I2S capture task, and draw_footer() does a vTaskDelay
     * (battery settle), a FAT free-cluster scan on the same SD bus that is
     * writing the WAV, and extra SPI blits — enough to overrun the ~90 ms I2S
     * DMA ring and punch silent gaps into the recording. Battery/storage are
     * already painted once when the RECORDING screen is drawn; that is enough. */
    draw_timer(elapsed_s);
#endif
}

void status_tick_idle(void)
{
#if BSP_HAS_LCD
    if (!s_lcd_ready || s_panel == NULL || !s_display_on) {
        return;
    }
    switch (s_status) {
    case OPEN_PLAUD_STATUS_READY:
    case OPEN_PLAUD_STATUS_SAVED:
    case OPEN_PLAUD_STATUS_DONE:
    case OPEN_PLAUD_STATUS_OFFLINE:
        draw_footer();
        break;
    default:
        break;
    }
#endif
}

void status_display(bool on)
{
#if BSP_HAS_LCD
    if (!s_lcd_ready) {
        return;
    }
    if (on == s_display_on) {
        return;
    }
    s_display_on = on;
    gpio_set_level(BSP_LCD_BL_GPIO, on ? 1 : 0);
    if (on) {
        lcd_draw_full(s_status, &s_info);
    }
#else
    (void)on;
#endif
}

void status_backlight(bool on)
{
#if BSP_HAS_LCD
    if (!s_lcd_ready || on == s_display_on) {
        return;
    }
    s_display_on = on;
    gpio_set_level(BSP_LCD_BL_GPIO, on ? 1 : 0);
#else
    (void)on;
#endif
}
