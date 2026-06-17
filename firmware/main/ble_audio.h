#pragma once

#include <stddef.h>
#include <stdint.h>

/* Stateless IMA ADPCM encoder used for BLE chunk transfer. Each BLE chunk
 * carries its own predictor/index header so an iPhone can resume at any source
 * byte offset without replaying earlier chunks. */
size_t open_plaud_ble_adpcm_encode_block(const int16_t *pcm, size_t samples,
                                         uint8_t *out, size_t out_cap);

