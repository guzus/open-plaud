/*
 * open-plaud recorder — bring-up firmware.
 *
 * Boot milestone: mount SD -> init ES7210 -> record a fixed clip to a WAV on the
 * card -> log it. This is the "record to SD" half of the device's job; Wi-Fi
 * upload to the server is the next milestone (net_upload.c is a stub today).
 */
#include <stdint.h>
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio.h"
#include "net_upload.h"
#include "sdcard.h"
#include "wav.h"

static const char *TAG = "open-plaud";

#define REC_SECONDS 5
#define REC_PATH    SDCARD_MOUNT_POINT "/rec_0001.wav"

void app_main(void)
{
    ESP_LOGI(TAG, "open-plaud recorder boot");

    esp_err_t nvs = nvs_flash_init();           /* needed later for Wi-Fi creds */
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (sdcard_mount() != ESP_OK) { ESP_LOGE(TAG, "no SD card — halting"); return; }
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed — halting");
        sdcard_unmount();
        return;
    }

    wav_writer_t wav;
    if (wav_open(&wav, REC_PATH, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open %s", REC_PATH);
        audio_deinit();
        sdcard_unmount();
        return;
    }

    const size_t target =
        (size_t)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8) * REC_SECONDS;
    static int16_t buf[2048];
    size_t written = 0, got = 0;

    ESP_LOGI(TAG, "recording %d s -> %s", REC_SECONDS, REC_PATH);
    while (written < target) {
        size_t want = sizeof buf;
        if (want > target - written) want = target - written;   /* exact length on the last read */
        if (audio_read(buf, want, &got) != ESP_OK || got == 0) continue;
        if (wav_write(&wav, buf, got) != ESP_OK) {  /* SD write failed — card trouble */
            ESP_LOGE(TAG, "SD write failed — aborting recording");
            break;
        }
        written += got;
    }
    /* wav_close patches the RIFF/data sizes in the header. If that fails, the file
     * still carries the open-time placeholder (data_bytes = 0) and is corrupt — do
     * not upload it; leave it on the card for inspection. */
    if (wav_close(&wav) != ESP_OK) {
        ESP_LOGE(TAG, "wav close failed — not uploading corrupt clip; %s left on SD", REC_PATH);
        audio_deinit();
        sdcard_unmount();
        return;
    }
    ESP_LOGI(TAG, "wrote %u bytes to %s", (unsigned) written, REC_PATH);

    esp_err_t upload = net_upload_wav(REC_PATH);
    if (upload == ESP_OK) {
        ESP_LOGI(TAG, "uploaded %s", REC_PATH);
    } else {
        ESP_LOGW(TAG, "upload skipped/failed (%s); %s remains on SD",
                 esp_err_to_name(upload), REC_PATH);
    }

    ESP_LOGI(TAG, "done. next: hold-to-record (PLUS button) + retry queue");
    audio_deinit();
    sdcard_unmount();
}
