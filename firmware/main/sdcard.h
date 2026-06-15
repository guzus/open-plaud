#pragma once
#include <stdint.h>
#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/* Mount the microSD card at /sdcard (FATFS over 1-bit SDMMC). */
esp_err_t sdcard_mount(void);
void      sdcard_unmount(void);

/* Card usage in bytes (either out pointer may be NULL). Fails if not mounted. */
esp_err_t sdcard_usage(uint64_t *out_used, uint64_t *out_total);
