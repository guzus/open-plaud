#pragma once

#include "esp_err.h"

typedef enum {
    OPEN_PLAUD_STATUS_BOOT,
    OPEN_PLAUD_STATUS_READY,
    OPEN_PLAUD_STATUS_RECORDING,
    OPEN_PLAUD_STATUS_SYNCING,
    OPEN_PLAUD_STATUS_DONE,
    OPEN_PLAUD_STATUS_ERROR,
} open_plaud_status_t;

esp_err_t status_init(void);
void status_show(open_plaud_status_t status);
