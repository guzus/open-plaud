#include "opus_ogg.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "opus.h"

static const char *TAG = "opus_ogg";

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t ogg_crc_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

static esp_err_t ogg_write_page(FILE *out, uint32_t serial, uint32_t *sequence,
                                uint8_t header_type, uint64_t granule_pos,
                                const uint8_t *segments, int segment_count,
                                const uint8_t *payload, size_t payload_len)
{
    if (segment_count < 0 || segment_count > OPUS_OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t header[27 + OPUS_OGG_MAX_SEGMENTS];
    memcpy(header, "OggS", 4);
    header[4] = 0;
    header[5] = header_type;
    put_le64(header + 6, granule_pos);
    put_le32(header + 14, serial);
    put_le32(header + 18, (*sequence)++);
    put_le32(header + 22, 0);
    header[26] = (uint8_t)segment_count;
    if (segment_count > 0) {
        memcpy(header + 27, segments, (size_t)segment_count);
    }

    uint32_t crc = ogg_crc_update(0, header, 27U + (size_t)segment_count);
    crc = ogg_crc_update(crc, payload, payload_len);
    put_le32(header + 22, crc);

    if (fwrite(header, 1, 27U + (size_t)segment_count, out) !=
        27U + (size_t)segment_count) {
        return ESP_FAIL;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, out) != payload_len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ogg_write_single_packet(FILE *out, uint32_t serial,
                                         uint32_t *sequence, uint8_t header_type,
                                         uint64_t granule_pos,
                                         const uint8_t *packet, size_t packet_len)
{
    uint8_t segments[OPUS_OGG_MAX_SEGMENTS];
    int segment_count = 0;
    size_t remaining = packet_len;
    while (remaining >= 255) {
        if (segment_count >= OPUS_OGG_MAX_SEGMENTS) {
            return ESP_ERR_INVALID_SIZE;
        }
        segments[segment_count++] = 255;
        remaining -= 255;
    }
    if (segment_count >= OPUS_OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    segments[segment_count++] = (uint8_t)remaining;

    return ogg_write_page(out, serial, sequence, header_type, granule_pos,
                          segments, segment_count, packet, packet_len);
}

static esp_err_t ogg_flush_audio_page(opus_ogg_writer_t *w, uint8_t header_type)
{
    if (w->segment_count == 0 && header_type == 0) {
        return ESP_OK;
    }
    esp_err_t err = ogg_write_page(w->file, w->serial, &w->sequence, header_type,
                                   w->granule_pos, w->segments, w->segment_count,
                                   w->payload, w->payload_len);
    w->segment_count = 0;
    w->payload_len = 0;
    return err;
}

static int opus_lacing_segments(size_t packet_len)
{
    return (int)(packet_len / 255) + 1;
}

static esp_err_t ogg_queue_audio_packet(opus_ogg_writer_t *w, const uint8_t *packet,
                                        size_t packet_len, uint64_t granule_pos)
{
    int need_segments = opus_lacing_segments(packet_len);
    if (need_segments > OPUS_OGG_MAX_SEGMENTS ||
        packet_len > OPUS_OGG_PAGE_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (w->segment_count + need_segments > OPUS_OGG_MAX_SEGMENTS ||
        w->payload_len + packet_len > OPUS_OGG_PAGE_PAYLOAD_BYTES) {
        esp_err_t err = ogg_flush_audio_page(w, 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    size_t remaining = packet_len;
    while (remaining >= 255) {
        w->segments[w->segment_count++] = 255;
        remaining -= 255;
    }
    w->segments[w->segment_count++] = (uint8_t)remaining;
    memcpy(w->payload + w->payload_len, packet, packet_len);
    w->payload_len += packet_len;
    w->granule_pos = granule_pos;
    return ESP_OK;
}

static esp_err_t ogg_write_opus_headers(FILE *out, uint32_t serial,
                                        uint32_t *sequence,
                                        uint32_t input_sample_rate)
{
    uint8_t head[19] = { 0 };
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 1;
    put_le16(head + 10, 0);
    put_le32(head + 12, input_sample_rate);
    put_le16(head + 16, 0);
    head[18] = 0;
    esp_err_t err = ogg_write_single_packet(out, serial, sequence, 0x02, 0,
                                            head, sizeof head);
    if (err != ESP_OK) {
        return err;
    }

    const char vendor[] = "open-plaud";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_le32(tags + 12 + sizeof(vendor) - 1, 0);
    return ogg_write_single_packet(out, serial, sequence, 0, 0, tags, sizeof tags);
}

static bool opus_supported_rate(uint32_t sample_rate)
{
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
           sample_rate == 24000 || sample_rate == 48000;
}

static esp_err_t replace_wav_extension(const char *wav_path, const char *suffix,
                                       char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(wav_path && suffix && out && out_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad path args");
    const char *base = strrchr(wav_path, '/');
    const char *name = base ? base + 1 : wav_path;
    const char *dot = strrchr(name, '.');
    size_t prefix_len = dot && dot > name ? (size_t)(dot - wav_path) : strlen(wav_path);
    int n = snprintf(out, out_len, "%.*s%s", (int)prefix_len, wav_path, suffix);
    return (n < 0 || (size_t)n >= out_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t opus_ogg_path_for_wav(const char *wav_path, char *out, size_t out_len)
{
    return replace_wav_extension(wav_path, ".opus", out, out_len);
}

esp_err_t opus_ogg_temp_path_for_wav(const char *wav_path, char *out, size_t out_len)
{
    return replace_wav_extension(wav_path, ".opus.tmp", out, out_len);
}

esp_err_t opus_ogg_upload_filename(const char *wav_path, char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(wav_path && out && out_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad filename args");
    const char *base = strrchr(wav_path, '/');
    base = base ? base + 1 : wav_path;
    const char *dot = strrchr(base, '.');
    int stem_len = dot && dot > base ? (int)(dot - base) : (int)strlen(base);
    int n = snprintf(out, out_len, "%.*s.opus", stem_len, base);
    return (n < 0 || (size_t)n >= out_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t encode_buffered_frame(opus_ogg_writer_t *w, uint32_t actual_samples)
{
    if (w->buffered_samples < w->frame_samples) {
        memset(w->pcm + w->buffered_samples, 0,
               (w->frame_samples - w->buffered_samples) * sizeof(w->pcm[0]));
    }

    int packet_len = opus_encode((OpusEncoder *)w->encoder, w->pcm,
                                 (int)w->frame_samples, w->packet,
                                 sizeof(w->packet));
    if (packet_len < 0) {
        ESP_LOGW(TAG, "Opus encode failed: %d", packet_len);
        return ESP_FAIL;
    }

    w->processed_samples += actual_samples;
    uint64_t granule = (w->processed_samples * 48000U) / w->sample_rate;
    esp_err_t err = ogg_queue_audio_packet(w, w->packet, (size_t)packet_len, granule);
    w->buffered_samples = 0;
    return err;
}

esp_err_t opus_ogg_open(opus_ogg_writer_t *w, const char *path,
                        uint32_t sample_rate, int bitrate)
{
    ESP_RETURN_ON_FALSE(w && path, ESP_ERR_INVALID_ARG, TAG, "bad open args");
    ESP_RETURN_ON_FALSE(opus_supported_rate(sample_rate), ESP_ERR_INVALID_ARG,
                        TAG, "unsupported Opus sample rate");

    memset(w, 0, sizeof(*w));
    w->sample_rate = sample_rate;
    w->frame_samples = sample_rate / (1000 / OPUS_OGG_FRAME_MS);
    ESP_RETURN_ON_FALSE(w->frame_samples > 0 && w->frame_samples <= 960,
                        ESP_ERR_INVALID_ARG, TAG, "bad Opus frame size");

    FILE *out = fopen(path, "wb");
    if (!out) {
        ESP_LOGW(TAG, "cannot create Opus file %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    int opus_err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create((opus_int32)sample_rate, 1,
                                           OPUS_APPLICATION_VOIP, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        if (enc) {
            opus_encoder_destroy(enc);
        }
        fclose(out);
        remove(path);
        ESP_LOGW(TAG, "Opus encoder create failed: %d", opus_err);
        return ESP_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    w->file = out;
    w->encoder = enc;
    w->serial = esp_random();
    esp_err_t err = ogg_write_opus_headers(out, w->serial, &w->sequence, sample_rate);
    if (err != ESP_OK) {
        opus_ogg_abort(w, path);
        return err;
    }
    w->open = true;
    return ESP_OK;
}

esp_err_t opus_ogg_write_pcm(opus_ogg_writer_t *w, const int16_t *mono,
                             size_t samples)
{
    ESP_RETURN_ON_FALSE(w && w->open && mono, ESP_ERR_INVALID_ARG,
                        TAG, "bad pcm args");
    while (samples > 0) {
        size_t room = w->frame_samples - w->buffered_samples;
        size_t n = samples < room ? samples : room;
        memcpy(w->pcm + w->buffered_samples, mono, n * sizeof(mono[0]));
        w->buffered_samples += (uint32_t)n;
        mono += n;
        samples -= n;
        if (w->buffered_samples == w->frame_samples) {
            esp_err_t err = encode_buffered_frame(w, w->frame_samples);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t opus_ogg_close(opus_ogg_writer_t *w, long *out_size)
{
    ESP_RETURN_ON_FALSE(w && w->open, ESP_ERR_INVALID_ARG, TAG, "bad close args");
    esp_err_t err = ESP_OK;
    if (w->buffered_samples > 0) {
        err = encode_buffered_frame(w, w->buffered_samples);
    }
    if (err == ESP_OK) {
        err = ogg_flush_audio_page(w, 0x04);
    }
    if (err == ESP_OK && fflush(w->file) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fseek(w->file, 0, SEEK_END) == 0) {
        long size = ftell(w->file);
        if (size > 0) {
            if (out_size) {
                *out_size = size;
            }
        } else {
            err = ESP_FAIL;
        }
    } else if (err == ESP_OK) {
        err = ESP_FAIL;
    }

    FILE *file = w->file;
    OpusEncoder *enc = (OpusEncoder *)w->encoder;
    w->file = NULL;
    w->encoder = NULL;
    w->open = false;
    if (enc) {
        opus_encoder_destroy(enc);
    }
    if (file && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

void opus_ogg_abort(opus_ogg_writer_t *w, const char *path)
{
    if (!w) {
        return;
    }
    FILE *file = w->file;
    OpusEncoder *enc = (OpusEncoder *)w->encoder;
    w->file = NULL;
    w->encoder = NULL;
    w->open = false;
    if (enc) {
        opus_encoder_destroy(enc);
    }
    if (file) {
        fclose(file);
    }
    if (path) {
        remove(path);
    }
}
