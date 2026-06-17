#include "ble_audio.h"

#include <stdbool.h>

static const int s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint8_t encode_sample(int sample, int *predictor, int *index)
{
    int step = s_step_table[*index];
    int diff = sample - *predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    int delta = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        delta += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        diff -= step >> 1;
        delta += step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        delta += step >> 2;
    }

    if (code & 8) {
        *predictor -= delta;
    } else {
        *predictor += delta;
    }
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;

    *index += s_index_table[code & 0x0f];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return code & 0x0f;
}

size_t open_plaud_ble_adpcm_encode_block(const int16_t *pcm, size_t samples,
                                         uint8_t *out, size_t out_cap)
{
    if (!pcm || !out || samples == 0 || out_cap < 4) {
        return 0;
    }

    int predictor = pcm[0];
    int index = 0;
    put_le16(out, (uint16_t)predictor);
    out[2] = (uint8_t)index;
    out[3] = 0;

    size_t pos = 4;
    uint8_t packed = 0;
    bool low = true;
    for (size_t i = 1; i < samples; i++) {
        uint8_t nibble = encode_sample(pcm[i], &predictor, &index);
        if (low) {
            packed = nibble;
            low = false;
        } else {
            if (pos >= out_cap) {
                return 0;
            }
            out[pos++] = packed | (uint8_t)(nibble << 4);
            low = true;
        }
    }
    if (!low) {
        if (pos >= out_cap) {
            return 0;
        }
        out[pos++] = packed;
    }
    return pos;
}

