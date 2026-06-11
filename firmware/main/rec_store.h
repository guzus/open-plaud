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

/* Upload pending clips oldest-first, moving each success into /sent. Stops on
 * the first upload failure (Wi-Fi is likely down — don't burn the boot on N
 * timeouts) and reports it via *out_upload_failed so the caller can skip
 * uploads for the rest of this boot. Returns the number of clips uploaded. */
int rec_store_retry_pending(bool *out_upload_failed);
