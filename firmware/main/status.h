#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OPEN_PLAUD_STATUS_BOOT,
    OPEN_PLAUD_STATUS_READY,
    OPEN_PLAUD_STATUS_RECORDING,
    OPEN_PLAUD_STATUS_SAVED,     /* clip(s) stopped + queued, sync deferred */
    OPEN_PLAUD_STATUS_SYNCING,
    OPEN_PLAUD_STATUS_DONE,
    OPEN_PLAUD_STATUS_OFFLINE,   /* sync attempted but clips still queued   */
    OPEN_PLAUD_STATUS_ERROR,
} open_plaud_status_t;

/* Dynamic figures drawn alongside a state. A negative int / NULL pointer means
 * "hide this field". Battery and storage are read by status.c itself, so they
 * are not passed in here. */
typedef struct {
    int pending;      /* clips queued to sync (READY/SAVED/OFFLINE)      */
    int elapsed_s;    /* recording elapsed seconds (RECORDING)           */
    int sync_done;    /* clips uploaded so far this sync (SYNCING/DONE)  */
    int sync_total;   /* clips this sync started with                    */
    const char *clip; /* active clip basename, e.g. "REC_0007"           */
} status_info_t;

esp_err_t status_init(void);

/* Full-screen redraw for a state transition. status_show() hides all the
 * dynamic fields; status_update() shows the ones you populate. */
void status_show(open_plaud_status_t status);
void status_update(open_plaud_status_t status, const status_info_t *info);

/* Cheap partial redraws. These repaint only a small region (timer / footer),
 * NOT the whole screen, so they are safe to call from inside the recording
 * loop without stalling I2S capture. Each is a no-op unless the current state
 * matches. */
void status_tick_recording(int elapsed_s);
void status_tick_idle(void);

/* Power button: blank or restore the panel backlight without tearing down the
 * display pipeline. status_display(true) repaints the current screen. */
void status_display(bool on);
