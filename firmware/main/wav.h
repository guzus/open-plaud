#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

/* Minimal streaming 16-bit PCM WAV writer. Writes a placeholder 44-byte header
 * on open, appends PCM, and patches the size fields on close. */
typedef struct {
    FILE    *f;
    uint32_t data_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
} wav_writer_t;

esp_err_t wav_open(wav_writer_t *w, const char *path,
                   uint32_t sample_rate, uint16_t channels, uint16_t bits);
esp_err_t wav_write(wav_writer_t *w, const void *pcm, size_t bytes);
esp_err_t wav_close(wav_writer_t *w);
