#pragma once
#include "esp_err.h"

/* Upload a finished WAV to the open-plaud server (multipart POST /v1/recordings).
 * Credentials and server URL come from idf.py menuconfig -> open-plaud. */
esp_err_t net_upload_wav(const char *path);
