#pragma once
#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/* Mount the microSD card at /sdcard (FATFS over 1-bit SDMMC). */
esp_err_t sdcard_mount(void);
void      sdcard_unmount(void);
