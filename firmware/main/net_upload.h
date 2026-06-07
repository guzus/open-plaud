#pragma once
#include "esp_err.h"

/* Upload a finished WAV to the open-plaud server (multipart POST /v1/recordings).
 * Stub today — see firmware/README.md "Device contract" for the target shape. */
esp_err_t net_upload_wav(const char *path);
