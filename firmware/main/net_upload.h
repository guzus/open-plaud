#pragma once
#include "esp_err.h"

typedef enum {
    NET_UPLOAD_PHASE_WIFI_CONNECTING,
    NET_UPLOAD_PHASE_WIFI_CONNECTED,
    NET_UPLOAD_PHASE_WIFI_FAILED,
    NET_UPLOAD_PHASE_UPLOADING,
} net_upload_phase_t;

/* Fired while a WAV body is being uploaded. `sent_bytes` counts file bytes only,
 * not multipart headers/tail. For Wi-Fi phases, byte counts are zero. May be NULL. */
typedef void (*net_upload_progress_fn)(const char *path, int sent_bytes,
                                       int total_bytes, net_upload_phase_t phase,
                                       void *ctx);

/* Upload a finished WAV to the open-plaud server (multipart POST /v1/recordings).
 * Credentials and server URL come from idf.py menuconfig -> open-plaud. */
esp_err_t net_upload_wav(const char *path, net_upload_progress_fn progress,
                         void *ctx);

/* Stop the Wi-Fi station after an upload batch to cut idle draw. The driver
 * stays initialized so the next upload reconnects without rebuilding the stack. */
void net_upload_shutdown(void);
