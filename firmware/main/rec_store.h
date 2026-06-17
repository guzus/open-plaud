#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* SD-side clip bookkeeping. rec_NNNN.wav in the card root means "recorded,
 * not yet uploaded"; uploaded clips move to /sdcard/sent. Numbering scans both
 * places so a number is never reused and a clip is never overwritten. */
esp_err_t rec_store_init(void);                            /* mkdir /sdcard/sent       */
esp_err_t rec_store_next_path(char *out, size_t out_len);  /* next free rec_NNNN.wav   */
esp_err_t rec_store_mark_sent(const char *path);           /* move a clip into /sent   */
int       rec_store_pending_count(void);                   /* rec_*.wav awaiting upload */

typedef enum {
    REC_STORE_PROGRESS_WIFI_CONNECTING,
    REC_STORE_PROGRESS_WIFI_CONNECTED,
    REC_STORE_PROGRESS_WIFI_FAILED,
    REC_STORE_PROGRESS_UPLOADING,
    REC_STORE_PROGRESS_CLIP_DONE,
} rec_store_progress_phase_t;

/* Fired during upload with (completed clips, total clips, active file percent,
 * active clip path). `current_pct` is 0..100 while a clip is in flight and -1
 * after a whole-clip success. May be NULL. Runs on the caller's task — safe for a
 * UI redraw during Sync, since no audio capture is in flight then. */
typedef void (*rec_store_progress_fn)(int uploaded, int total, int current_pct,
                                      const char *clip,
                                      rec_store_progress_phase_t phase,
                                      void *ctx);

/* Upload pending clips oldest-first, moving each success into /sent. Stops on
 * the first upload failure (Wi-Fi is likely down — don't burn the boot on N
 * timeouts) and reports it via *out_upload_failed so the caller can leave the
 * remaining clips queued for a later retry. Returns the number uploaded.
 * `progress` (nullable) is called after each success for live UI. */
int rec_store_retry_pending(bool *out_upload_failed,
                            rec_store_progress_fn progress, void *ctx);
