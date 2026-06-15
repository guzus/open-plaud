/*
 * open-plaud recorder — 3-button firmware.
 *
 * Boot: mount SD -> clip store -> audio/buttons/power/status -> auto-retry any
 * clips a previous run failed to upload (uploads happen on boot if Wi-Fi is up).
 * Then poll three top-edge buttons forever:
 *
 *   Right  (record) : tap to start recording, tap again to stop. Stop SAVES the
 *                     clip and QUEUES it — it does NOT upload (deferred sync).
 *   Middle (sync)   : upload every queued clip, oldest first.
 *   Left   (power)  : screen on/off when idle; Cancel (discard) while recording.
 *
 * Clips are rec_NNNN.wav in the card root (numbered, never reused); uploaded
 * clips move to /sdcard/sent. The device stays dumb on purpose
 * (docs/ARCHITECTURE.md): record locally, upload raw audio, let the server do
 * the thinking. The physical Left/Middle button mapping is unverified — see
 * board.h; this file logs the GPIO behind each label at boot.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "board.h"
#include "button.h"
#include "net_upload.h"
#include "power.h"
#include "rec_store.h"
#include "sdcard.h"
#include "status.h"
#include "wav.h"

static const char *TAG = "open-plaud";

#define BTN_POLL_MS        20
#define BTN_DEBOUNCE_POLLS 2
#define IDLE_REFRESH_MS    2000   /* battery/storage footer refresh cadence */
#define BYTES_PER_SEC      (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8))

typedef enum { REC_SAVED, REC_CANCELLED, REC_FAILED } record_result_t;

/* Debounced press-edge detection for the polled buttons. */
typedef struct {
    bool down;   /* debounced state */
    int  agree;  /* consecutive raw reads disagreeing with `down` */
} btn_state_t;

static btn_state_t s_btn[BTN_COUNT];
static bool s_screen_on = true;

/* Returns true exactly once, on the poll where `id` becomes debounced-pressed. */
static bool btn_poll_edge(button_id_t id)
{
    const bool raw = button_is_down(id);
    btn_state_t *b = &s_btn[id];
    if (raw != b->down) {
        if (++b->agree >= BTN_DEBOUNCE_POLLS) {
            b->down = raw;
            b->agree = 0;
            return b->down;
        }
    } else {
        b->agree = 0;
    }
    return false;
}

/* Re-baseline the debounced states to the live levels without emitting edges —
 * call after a blocking action (record/sync) so a still-held button does not
 * register as a fresh press. */
static void btn_resync(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn[i].down = button_is_down(i);
        s_btn[i].agree = 0;
    }
}

static void ensure_screen_on(void)
{
    if (!s_screen_on) {
        s_screen_on = true;
        status_display(true);
    }
}

static void retry_pending_clips(const char *context)
{
    bool upload_failed = false;
    int cleared = rec_store_retry_pending(&upload_failed, NULL, NULL);
    ESP_LOGI(TAG, "%s retry queue: %d pending clip(s) uploaded", context, cleared);
    if (upload_failed) {
        ESP_LOGW(TAG, "%s retry queue not fully drained; clips stay queued on SD",
                 context);
    }
}

static void show_idle(int pending)
{
    const status_info_t info = {
        .pending = pending, .elapsed_s = -1,
        .sync_done = -1, .sync_total = -1, .clip = NULL,
    };
    status_update(pending > 0 ? OPEN_PLAUD_STATUS_SAVED : OPEN_PLAUD_STATUS_READY, &info);
}

/* "rec_0007.wav" -> "REC 0007": the on-screen font is uppercase digits/letters
 * only, so drop the extension and turn '_' into a space. */
static void clip_label(const char *base, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; base[i] != '\0' && j + 1 < n; i++) {
        char c = base[i];
        if (c == '.') {
            break;
        }
        if (c == '_') {
            c = ' ';
        } else if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

/* Record one clip to `path`. Toggle semantics: recording was started by a Right
 * press, so we wait for Right to be released, then a second Right press stops
 * and SAVES. A Left press cancels (discards). Auto-stops at
 * CONFIG_OPEN_PLAUD_MAX_REC_SECONDS or on a sustained audio stall. The on-screen
 * RECORDING card is drawn by the caller; here we only tick the timer region
 * (cheap partial redraw) so capture is never stalled by a full repaint.
 */
static record_result_t record_clip(const char *path)
{
    wav_writer_t wav;
    if (wav_open(&wav, path, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return REC_FAILED;
    }

    const size_t max_bytes = (size_t)BYTES_PER_SEC * CONFIG_OPEN_PLAUD_MAX_REC_SECONDS;
    static int16_t buf[2048];           /* 4 KiB ~= 64 ms at 16 kHz stereo */
    size_t written = 0;
    int bad_reads = 0;
    int elapsed_shown = -1;

    bool stop_armed = false;            /* Right released since the start press */
    int right_rel = 0, right_press = 0, left_press = 0;
    record_result_t result = REC_SAVED;

    ESP_LOGI(TAG, "recording %s (tap Right to stop, Left to cancel)", path);
    while (written < max_bytes) {
        /* Poll buttons every iteration, BEFORE the audio read, so stop/cancel is
         * honored even when the mic wedges — a slow or empty read must never
         * trap the user in a recording. Cadence is ~64 ms on healthy reads and
         * ~the read timeout during a stall. */
        const bool right = button_is_down(BTN_RECORD);
        const bool left = button_is_down(BTN_POWER);
        if (!stop_armed) {
            right_rel = right ? 0 : right_rel + 1;
            if (right_rel >= BTN_DEBOUNCE_POLLS) {
                stop_armed = true;          /* Right released since the start press */
            }
        } else {
            right_press = right ? right_press + 1 : 0;
            if (right_press >= BTN_DEBOUNCE_POLLS) {
                ESP_LOGI(TAG, "stop pressed");
                break;
            }
        }
        /* Cancel only once the recording is established (Right released). This
         * also makes a board where Left aliases Right degrade safely: Stop is
         * checked first and wins, so it can record but not cancel. */
        left_press = left ? left_press + 1 : 0;
        if (stop_armed && left_press >= BTN_DEBOUNCE_POLLS) {
            ESP_LOGI(TAG, "cancel pressed");
            result = REC_CANCELLED;
            break;
        }

        size_t got = 0;
        size_t want = sizeof buf;
        if (want > max_bytes - written) {
            want = max_bytes - written;
        }

        esp_err_t read_err = audio_read(buf, want, &got);
        if (got == 0) {
            if (read_err != ESP_OK) {
                ESP_LOGW(TAG, "audio read failed/empty (%d/%d): %s",
                         bad_reads + 1, CONFIG_OPEN_PLAUD_AUDIO_READ_MAX_ERRORS,
                         esp_err_to_name(read_err));
            }
            if (++bad_reads >= CONFIG_OPEN_PLAUD_AUDIO_READ_MAX_ERRORS) {
                ESP_LOGE(TAG, "audio stalled (%d empty reads) - stopping clip", bad_reads);
                break;
            }
            continue;
        }

        bad_reads = 0;
        if (wav_write(&wav, buf, got) != ESP_OK) {
            ESP_LOGE(TAG, "SD write failed - stopping clip");
            break;
        }
        written += got;

        const int elapsed = (int)(written / BYTES_PER_SEC);
        if (elapsed != elapsed_shown) {
            elapsed_shown = elapsed;
            status_tick_recording(elapsed);
        }
    }
    if (written >= max_bytes) {
        ESP_LOGW(TAG, "max clip length (%d s) reached - auto-stop",
                 CONFIG_OPEN_PLAUD_MAX_REC_SECONDS);
    }

    const bool close_ok = (wav_close(&wav) == ESP_OK);
    if (result == REC_CANCELLED || written == 0) {
        remove(path);
        ESP_LOGI(TAG, "%s removed (%s)", path,
                 result == REC_CANCELLED ? "cancelled" : "no audio captured");
        return result == REC_CANCELLED ? REC_CANCELLED : REC_FAILED;
    }
    if (!close_ok) {
        ESP_LOGE(TAG, "wav close failed - not keeping corrupt clip; %s left on SD", path);
        return REC_FAILED;
    }
    ESP_LOGI(TAG, "saved %u bytes to %s (queued for sync)", (unsigned)written, path);
    return REC_SAVED;
}

/* Right button: record one clip (toggle), then leave it queued. No upload. */
static void do_record(void)
{
    char path[32];
    if (rec_store_next_path(path, sizeof path) != ESP_OK) {
        ESP_LOGE(TAG, "no usable clip name - recording skipped");
        status_show(OPEN_PLAUD_STATUS_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char label[16];
    clip_label(base, label, sizeof label);

    const status_info_t info = {
        .pending = -1, .elapsed_s = 0,
        .sync_done = -1, .sync_total = -1, .clip = label,
    };
    status_update(OPEN_PLAUD_STATUS_RECORDING, &info);

    const record_result_t r = record_clip(path);
    if (r == REC_FAILED) {
        status_show(OPEN_PLAUD_STATUS_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

/* Redraw the SYNCING screen after each clip uploads. Safe to redraw fully here:
 * sync runs on the main loop, not the capture task, so no audio is in flight. */
static void sync_progress(int uploaded, int total, void *ctx)
{
    (void)ctx;
    const status_info_t info = {
        .pending = total - uploaded, .elapsed_s = -1,
        .sync_done = uploaded, .sync_total = total, .clip = NULL,
    };
    status_update(OPEN_PLAUD_STATUS_SYNCING, &info);
}

/* Middle button: upload the queue oldest-first. Blocks (Wi-Fi + uploads), which
 * is why no live Cancel is offered mid-sync (the hint row shows BUSY). */
static void do_sync(int pending)
{
    if (pending <= 0) {
        const status_info_t done = {
            .pending = 0, .elapsed_s = -1,
            .sync_done = 0, .sync_total = 0, .clip = NULL,
        };
        status_update(OPEN_PLAUD_STATUS_DONE, &done);
        return;
    }

    const status_info_t syncing = {
        .pending = pending, .elapsed_s = -1,
        .sync_done = 0, .sync_total = pending, .clip = NULL,
    };
    status_update(OPEN_PLAUD_STATUS_SYNCING, &syncing);

    bool failed = false;
    const int uploaded = rec_store_retry_pending(&failed, sync_progress, NULL);
    ESP_LOGI(TAG, "sync: %d uploaded%s", uploaded, failed ? " (stopped early)" : "");

    const int remaining = rec_store_pending_count();
    const status_info_t result = {
        .pending = remaining, .elapsed_s = -1,
        .sync_done = uploaded, .sync_total = pending, .clip = NULL,
    };
    status_update(remaining > 0 ? OPEN_PLAUD_STATUS_OFFLINE : OPEN_PLAUD_STATUS_DONE, &result);
}

void app_main(void)
{
    ESP_LOGI(TAG, "open-plaud recorder boot");
    if (status_init() == ESP_OK) {
        status_show(OPEN_PLAUD_STATUS_BOOT);
    }

    esp_err_t nvs = nvs_flash_init();           /* needed for Wi-Fi creds */
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no SD card - halting");
        status_show(OPEN_PLAUD_STATUS_ERROR);
        return;
    }
    if (rec_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "clip store init failed - halting");
        status_show(OPEN_PLAUD_STATUS_ERROR);
        sdcard_unmount();
        return;
    }
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed - halting");
        status_show(OPEN_PLAUD_STATUS_ERROR);
        sdcard_unmount();
        return;
    }
    if (button_init() != ESP_OK) {
        ESP_LOGE(TAG, "button init failed - halting");
        status_show(OPEN_PLAUD_STATUS_ERROR);
        audio_deinit();
        sdcard_unmount();
        return;
    }
    power_init();   /* battery sense is non-fatal: UI just shows "--" if absent */

    /* Bring-up aid: the Left/Middle physical mapping is unverified (board.h).
     * Press each button and confirm the GPIO logged here matches the label. */
    ESP_LOGW(TAG, "button map (UNVERIFIED Left/Middle): LEFT=GPIO%d (power/screen, cancel) "
                  "MIDDLE=GPIO%d (sync) RIGHT=GPIO%d (record/stop)",
             button_gpio(BTN_POWER), button_gpio(BTN_SYNC), button_gpio(BTN_RECORD));

    /* Drain the retry queue once per boot, before any new recording. Upload
     * failures are not latched: a later Sync press re-enters Wi-Fi setup. */
    status_show(OPEN_PLAUD_STATUS_SYNCING);
    retry_pending_clips("boot");

    btn_resync();
    int pending = rec_store_pending_count();
    show_idle(pending);
    ESP_LOGI(TAG, "ready - tap Right (GPIO%d) to record; %d clip(s) queued",
             button_gpio(BTN_RECORD), pending);

    TickType_t last_refresh = xTaskGetTickCount();
    while (true) {
        if (btn_poll_edge(BTN_RECORD)) {
            ensure_screen_on();
            do_record();
            btn_resync();
            pending = rec_store_pending_count();
            show_idle(pending);
            last_refresh = xTaskGetTickCount();
        } else if (btn_poll_edge(BTN_SYNC)) {
            ensure_screen_on();
            do_sync(pending);
            btn_resync();
            pending = rec_store_pending_count();
            last_refresh = xTaskGetTickCount();
        } else if (btn_poll_edge(BTN_POWER)) {
            s_screen_on = !s_screen_on;
            status_display(s_screen_on);
            ESP_LOGI(TAG, "screen %s", s_screen_on ? "on" : "off");
        } else if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(IDLE_REFRESH_MS)) {
            status_tick_idle();
            last_refresh = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}
