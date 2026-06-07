#include "net_upload.h"
#include "esp_log.h"

static const char *TAG = "net_upload";

/*
 * TODO(v1): Wi-Fi STA connect (esp_wifi, creds from NVS/menuconfig), then stream
 * `path` as multipart/form-data to  POST {OP_SERVER}/v1/recordings  using
 * esp_http_client. Contract lives in docs/ARCHITECTURE.md + server/open_plaud/api.
 * Kept as a stub so the record->SD path builds and runs on its own first.
 */
esp_err_t net_upload_wav(const char *path)
{
    ESP_LOGW(TAG, "upload not implemented yet — leaving %s on the SD card", path);
    return ESP_ERR_NOT_SUPPORTED;
}
