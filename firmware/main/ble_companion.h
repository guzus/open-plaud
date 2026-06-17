#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OPEN_PLAUD_BLE_COMMAND_NONE = 0,
    OPEN_PLAUD_BLE_COMMAND_SYNC,
} open_plaud_ble_command_t;

typedef struct {
    int sync_done;
    int sync_total;
    int upload_pct;
    bool wifi_connected;
    bool sync_failed;
} open_plaud_ble_status_t;

/* Starts the Open Plaud BLE companion service when CONFIG_OPEN_PLAUD_BLE_COMPANION=y. */
esp_err_t open_plaud_ble_start(void);
void open_plaud_ble_set_status(const open_plaud_ble_status_t *status);
bool open_plaud_ble_take_command(open_plaud_ble_command_t *out);
