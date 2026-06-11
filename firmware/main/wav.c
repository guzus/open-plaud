#include "wav.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "wav";

/* Build + write the canonical 44-byte PCM header (little-endian, as on ESP32).
 * Called once on open (sizes = 0) and again on close to patch the real sizes. */
static esp_err_t write_header(wav_writer_t *w)
{
    const uint32_t byte_rate   = w->sample_rate * w->channels * (w->bits / 8);
    const uint16_t block_align = w->channels * (w->bits / 8);
    const uint32_t riff_size   = 36 + w->data_bytes;
    uint8_t h[44];

    memcpy(h + 0, "RIFF", 4);
    h[4] = riff_size; h[5] = riff_size >> 8; h[6] = riff_size >> 16; h[7] = riff_size >> 24;
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;            /* fmt chunk size  */
    h[20] = 1;  h[21] = 0;                                  /* PCM             */
    h[22] = w->channels; h[23] = w->channels >> 8;
    h[24] = w->sample_rate; h[25] = w->sample_rate >> 8;
    h[26] = w->sample_rate >> 16; h[27] = w->sample_rate >> 24;
    h[28] = byte_rate; h[29] = byte_rate >> 8; h[30] = byte_rate >> 16; h[31] = byte_rate >> 24;
    h[32] = block_align; h[33] = block_align >> 8;
    h[34] = w->bits; h[35] = w->bits >> 8;
    memcpy(h + 36, "data", 4);
    h[40] = w->data_bytes; h[41] = w->data_bytes >> 8;
    h[42] = w->data_bytes >> 16; h[43] = w->data_bytes >> 24;

    if (fseek(w->f, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fwrite(h, 1, sizeof h, w->f) != sizeof h) return ESP_FAIL;
    return fseek(w->f, 0, SEEK_END) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t wav_open(wav_writer_t *w, const char *path,
                   uint32_t sample_rate, uint16_t channels, uint16_t bits)
{
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        return ESP_FAIL;
    }
    w->sample_rate = sample_rate;
    w->channels    = channels;
    w->bits        = bits;
    esp_err_t err = write_header(w);
    if (err != ESP_OK) {        /* don't leak the handle into the caller's loop */
        fclose(w->f);
        w->f = NULL;
    }
    return err;
}

esp_err_t wav_write(wav_writer_t *w, const void *pcm, size_t bytes)
{
    if (!w->f) return ESP_ERR_INVALID_STATE;
    if (fwrite(pcm, 1, bytes, w->f) != bytes) return ESP_FAIL;
    w->data_bytes += bytes;
    return ESP_OK;
}

esp_err_t wav_close(wav_writer_t *w)
{
    if (!w->f) return ESP_ERR_INVALID_STATE;
    esp_err_t err = write_header(w);   /* patch RIFF + data sizes */
    if (fclose(w->f) != 0) err = ESP_FAIL;
    w->f = NULL;
    return err;
}
