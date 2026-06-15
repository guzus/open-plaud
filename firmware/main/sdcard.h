#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/* Mount the microSD card at /sdcard (FATFS over SDMMC). `allow_format` controls
 * format-on-mount-failure: pass true only on a cold power-on (handles blank/exFAT
 * cards); pass false on a deep-sleep wake so a transient mount glitch can never
 * autoformat (wipe) a card full of recordings. */
esp_err_t sdcard_mount(bool allow_format);
void      sdcard_unmount(void);

/* Card usage in bytes (either out pointer may be NULL). Fails if not mounted. */
esp_err_t sdcard_usage(uint64_t *out_used, uint64_t *out_total);
