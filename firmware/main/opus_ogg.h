#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#define OPUS_OGG_MEDIA_TYPE "audio/ogg"
#define OPUS_OGG_FRAME_MS 20
#define OPUS_OGG_MAX_PACKET_BYTES 1500
#define OPUS_OGG_PACKETS_PER_PAGE 10
#define OPUS_OGG_MAX_SEGMENTS 255
#define OPUS_OGG_PAGE_PAYLOAD_BYTES (OPUS_OGG_MAX_PACKET_BYTES * OPUS_OGG_PACKETS_PER_PAGE)

typedef struct {
    FILE *file;
    void *encoder;
    uint32_t sample_rate;
    uint32_t frame_samples;
    uint32_t buffered_samples;
    uint64_t processed_samples;
    uint32_t serial;
    uint32_t sequence;
    uint8_t segments[OPUS_OGG_MAX_SEGMENTS];
    uint8_t payload[OPUS_OGG_PAGE_PAYLOAD_BYTES];
    int segment_count;
    size_t payload_len;
    uint64_t granule_pos;
    int16_t pcm[960]; /* 20 ms at the largest supported Opus rate, 48 kHz. */
    uint8_t packet[OPUS_OGG_MAX_PACKET_BYTES];
    bool open;
} opus_ogg_writer_t;

esp_err_t opus_ogg_path_for_wav(const char *wav_path, char *out, size_t out_len);
esp_err_t opus_ogg_temp_path_for_wav(const char *wav_path, char *out, size_t out_len);
esp_err_t opus_ogg_upload_filename(const char *wav_path, char *out, size_t out_len);

esp_err_t opus_ogg_open(opus_ogg_writer_t *w, const char *path,
                        uint32_t sample_rate, int bitrate);
esp_err_t opus_ogg_write_pcm(opus_ogg_writer_t *w, const int16_t *mono,
                             size_t samples);
esp_err_t opus_ogg_close(opus_ogg_writer_t *w, long *out_size);
void opus_ogg_abort(opus_ogg_writer_t *w, const char *path);
