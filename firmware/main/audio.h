#pragma once
#include <stddef.h>
#include "esp_err.h"

/* Dual-mic capture (ES7210 ADC over I2S TDM). */
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS        16
#define AUDIO_CHANNELS    2

esp_err_t audio_init(void);                                   /* I2C + I2S RX + ES7210 */
esp_err_t audio_read(void *buf, size_t bytes, size_t *out_read); /* blocking PCM read   */
void      audio_deinit(void);
