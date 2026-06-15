#include "net_upload.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net_upload";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define UPLOAD_CHUNK_BYTES 4096

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_wifi_netif;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_wifi_driver_ready;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_wifi_initialized;
static bool s_wifi_started;
static int s_retry_count;
static char s_upload_chunk[UPLOAD_CHUNK_BYTES];

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        int reason = event_data ? ((wifi_event_sta_disconnected_t *)event_data)->reason : -1;
        if (s_retry_count < CONFIG_OPEN_PLAUD_WIFI_MAX_RETRIES) {
            s_retry_count++;
            /* reason 201 = NO_AP_FOUND (wrong SSID/band/range); 15 = handshake
             * timeout / 2 = auth expire (usually a wrong password). */
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying (%d/%d)",
                     reason, s_retry_count, CONFIG_OPEN_PLAUD_WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_once(void)
{
    if (strlen(CONFIG_OPEN_PLAUD_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; configure it with idf.py menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_initialized) {
        return ESP_OK;
    }

    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(err, TAG, "netif_init");
        }
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(err, TAG, "event_loop");
        }
        s_event_loop_ready = true;
    }

    if (!s_wifi_netif) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_wifi_netif, ESP_FAIL, TAG, "wifi_netif");
    }

    if (!s_wifi_driver_ready) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi_init");
        s_wifi_driver_ready = true;
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "event_group");
    }

    if (!s_wifi_handler_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler, NULL),
                            TAG, "wifi_event_handler");
        s_wifi_handler_registered = true;
    }
    if (!s_ip_handler_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler, NULL),
                            TAG, "ip_event_handler");
        s_ip_handler_registered = true;
    }

    wifi_config_t wifi_cfg = { 0 };
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s",
             CONFIG_OPEN_PLAUD_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s",
             CONFIG_OPEN_PLAUD_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode =
        strlen(CONFIG_OPEN_PLAUD_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi_config");
    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t wifi_connect(void)
{
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once");

    EventBits_t bits = xEventGroupGetBits(s_wifi_events);
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    if (!s_wifi_started) {
        ESP_LOGI(TAG, "starting Wi-Fi STA");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");
        s_wifi_started = true;
    } else {
        ESP_LOGI(TAG, "reconnecting Wi-Fi STA");
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi_connect");
    }

    bits = xEventGroupWaitBits(s_wifi_events,
                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                               pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(CONFIG_OPEN_PLAUD_UPLOAD_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if ((bits & WIFI_FAIL_BIT) == 0) {
        ESP_LOGW(TAG, "Wi-Fi connect timed out after %d ms",
                 CONFIG_OPEN_PLAUD_UPLOAD_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

void net_upload_shutdown(void)
{
    if (!s_wifi_started) {
        return;
    }

    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Wi-Fi disconnect before stop failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(err));
        return;
    }

    s_wifi_started = false;
    s_retry_count = 0;
    ESP_LOGI(TAG, "Wi-Fi STA stopped");
}

static esp_err_t build_upload_url(char *buf, size_t len)
{
    const char *base = CONFIG_OPEN_PLAUD_SERVER_URL;
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    int written = snprintf(buf, len, "%.*s/v1/recordings", (int)base_len, base);
    if (written < 0 || (size_t)written >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t checked_write(esp_http_client_handle_t client, const char *data, int len)
{
    int total = 0;
    while (total < len) {
        int written = esp_http_client_write(client, data + total, len - total);
        if (written <= 0) {
            ESP_LOGE(TAG, "HTTP write failed after %d of %d bytes", total, len);
            return ESP_FAIL;
        }
        total += written;
    }
    return ESP_OK;
}

esp_err_t net_upload_wav(const char *path)
{
    ESP_RETURN_ON_FALSE(path, ESP_ERR_INVALID_ARG, TAG, "path is NULL");
    ESP_RETURN_ON_ERROR(wifi_connect(), TAG, "wifi_connect");

    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file, ESP_ERR_NOT_FOUND, TAG, "cannot open %s", path);

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    char url[256];
    esp_err_t err = build_upload_url(url, sizeof(url));
    if (err != ESP_OK) {
        fclose(file);
        ESP_LOGE(TAG, "server URL is too long");
        return err;
    }

    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    const char *boundary = "open-plaud-esp32s3";
    char head[256];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, filename);
    const char *tail_fmt = "\r\n--%s--\r\n";
    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail), tail_fmt, boundary);
    if (head_len < 0 || head_len >= (int)sizeof(head) ||
        tail_len < 0 || tail_len >= (int)sizeof(tail)) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_OPEN_PLAUD_UPLOAD_TIMEOUT_MS,
        // Verify TLS against the bundled root CAs so an https:// server URL
        // (e.g. Railway) works. Ignored for plain http:// (LAN) targets.
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(file);
        return ESP_FAIL;
    }

    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // Optional shared-secret auth: matches the server's OP_AUTH_TOKEN. Empty by
    // default (open server); set OPEN_PLAUD_AUTH_TOKEN in menuconfig to enable.
    if (strlen(CONFIG_OPEN_PLAUD_AUTH_TOKEN) > 0) {
        char auth[160];
        int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_OPEN_PLAUD_AUTH_TOKEN);
        if (auth_len > 0 && auth_len < (int)sizeof(auth)) {
            esp_http_client_set_header(client, "Authorization", auth);
        }
    }

    int64_t total_len = (int64_t)head_len + file_size + tail_len;
    if (total_len > INT_MAX) {
        fclose(file);
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "file is too large for esp_http_client_open content length");
        return ESP_ERR_INVALID_SIZE;
    }
    int content_len = (int)total_len;

    ESP_LOGI(TAG, "uploading %s (%ld bytes) -> %s", path, file_size, url);
    err = esp_http_client_open(client, content_len);
    if (err == ESP_OK) {
        err = checked_write(client, head, head_len);
    }

    while (err == ESP_OK) {
        size_t n = fread(s_upload_chunk, 1, sizeof(s_upload_chunk), file);
        if (n > 0) {
            err = checked_write(client, s_upload_chunk, (int)n);
        }
        if (n < sizeof(s_upload_chunk)) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
    }

    if (err == ESP_OK) {
        err = checked_write(client, tail, tail_len);
    }
    fclose(file);

    if (err == ESP_OK) {
        int headers = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "upload response: status=%d headers=%d", status, headers);
        if (status < 200 || status >= 300) {
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upload failed; leaving %s on the SD card", path);
    }
    return err;
}
