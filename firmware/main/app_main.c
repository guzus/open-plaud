/*
 * open-plaud recorder - hold-to-record firmware.
 *
 * Boot: mount SD -> clip store -> init audio/button/status -> retry any clips
 * a previous run failed to upload. Then loop forever: hold PLUS to record a
 * numbered WAV (rec_NNNN.wav, never reused), release to stop and upload.
 * Uploaded clips move to /sdcard/sent; failed uploads stay in the card root
 * for a later retry in this boot or the next boot. The device stays dumb on purpose
 * (docs/ARCHITECTURE.md): record locally, upload raw audio, let the server do
 * the thinking.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "board.h"
#include "button.h"
#include "net_upload.h"
#include "rec_store.h"
#include "sdcard.h"
#include "status.h"
#include "wav.h"

static const char *TAG = "open-plaud";

#define BTN_POLL_MS        20
#define BTN_DEBOUNCE_POLLS 2

/* Block until the button has read `pressed` for BTN_DEBOUNCE_POLLS consecutive
 * 20 ms polls. This is the only debouncing in the system: button.c reports raw
 * levels, with polarity supplied by board.h.
 */
static void wait_for_button(bool pressed)
{
    int agree = 0;
    while (agree < BTN_DEBOUNCE_POLLS) {
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
        agree = (button_pressed() == pressed) ? agree + 1 : 0;
    }
}

static void retry_pending_clips(const char *context)
{
    bool upload_failed = false;
    int cleared = rec_store_retry_pending(&upload_failed);
    ESP_LOGI(TAG, "%s retry queue: %d pending clip(s) uploaded", context, cleared);
    if (upload_failed) {
        ESP_LOGW(TAG, "%s retry queue not fully drained; clips stay queued on SD",
                 context);
    }
}

/* Record one clip to `path` while PLUS stays held; auto-stop at
 * CONFIG_OPEN_PLAUD_MAX_REC_SECONDS. Returns ESP_OK only when the file on SD is
 * a clean, non-empty, uploadable WAV.
 */
static esp_err_t record_clip(const char *path)
{
    wav_writer_t wav;
    if (wav_open(&wav, path, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }

    const size_t max_bytes = (size_t)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS *
                             (AUDIO_BITS / 8) * CONFIG_OPEN_PLAUD_MAX_REC_SECONDS;
    static int16_t buf[2048];           /* 4 KiB ~= 64 ms at 16 kHz stereo */
    size_t written = 0;
    int bad_reads = 0;
    int released = 0;

    ESP_LOGI(TAG, "recording %s (release PLUS to stop)", path);
    status_show(OPEN_PLAUD_STATUS_RECORDING);
    while (written < max_bytes) {
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

        /* Poll the button once per healthy chunk read (~64 ms). Two consecutive
         * released samples stop the clip.
         */
        released = button_pressed() ? 0 : released + 1;
        if (released >= BTN_DEBOUNCE_POLLS) {
            break;
        }
    }
    if (written >= max_bytes) {
        ESP_LOGW(TAG, "max clip length (%d s) reached - auto-stop",
                 CONFIG_OPEN_PLAUD_MAX_REC_SECONDS);
    }

    bool close_ok = (wav_close(&wav) == ESP_OK);
    if (written == 0) {
        remove(path);
        ESP_LOGW(TAG, "no audio captured - removed %s", path);
        return ESP_FAIL;
    }
    if (!close_ok) {
        ESP_LOGE(TAG, "wav close failed - not uploading corrupt clip; %s left on SD", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wrote %u bytes to %s", (unsigned)written, path);
    return ESP_OK;
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

    /* Drain the retry queue once per boot, before any new recording. Upload
     * failures are no longer latched for the whole boot: each later upload can
     * re-enter Wi-Fi setup and try again after the network or server recovers.
     */
    status_show(OPEN_PLAUD_STATUS_SYNCING);
    retry_pending_clips("boot");

    ESP_LOGI(TAG, "ready - hold PLUS (GPIO%d) to record", BSP_PLUS_BUTTON_GPIO);
    char path[32];
    while (true) {
        status_show(OPEN_PLAUD_STATUS_READY);
        wait_for_button(true);

        if (rec_store_next_path(path, sizeof path) != ESP_OK) {
            ESP_LOGE(TAG, "no usable clip name - recording skipped");
            status_show(OPEN_PLAUD_STATUS_ERROR);
        } else if (record_clip(path) == ESP_OK) {
            status_show(OPEN_PLAUD_STATUS_SYNCING);
            if (net_upload_wav(path) == ESP_OK) {
                ESP_LOGI(TAG, "uploaded %s", path);
                if (rec_store_mark_sent(path) != ESP_OK) {
                    ESP_LOGW(TAG, "%s uploaded but not moved to /sent - it may re-upload next boot", path);
                } else {
                    retry_pending_clips("post-upload");
                }
                status_show(OPEN_PLAUD_STATUS_DONE);
            } else {
                ESP_LOGW(TAG, "upload failed; %s queued on SD for retry", path);
                status_show(OPEN_PLAUD_STATUS_ERROR);
            }
        } else {
            status_show(OPEN_PLAUD_STATUS_ERROR);
        }

        wait_for_button(false);   /* require a release before re-arming */
    }
}
