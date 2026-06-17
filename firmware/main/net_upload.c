#include "net_upload.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "opus.h"

static const char *TAG = "net_upload";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define UPLOAD_CHUNK_BYTES 16384
#define WAV_HEADER_BYTES   44
#define OPAD_HEADER_BYTES  22
#define OPAD_MEDIA_TYPE    "application/vnd.openplaud.adpcm"
#define OGG_OPUS_MEDIA_TYPE "audio/ogg"
#define OPUS_FRAME_MS      20
#define OPUS_MAX_PACKET_BYTES 1500
#define OGG_PACKETS_PER_PAGE 10
#define OGG_MAX_SEGMENTS   255
#define OGG_PAGE_PAYLOAD_BYTES (OPUS_MAX_PACKET_BYTES * OGG_PACKETS_PER_PAGE)
#define CFG_NS             "op_ble_cfg"
#define KEY_SSID           "ssid"
#define KEY_WIFI_PW        "wifi_pw"
#define KEY_SERVER         "server"
#define KEY_TOKEN          "token"

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
static uint8_t s_upload_chunk[UPLOAD_CHUNK_BYTES];
static uint8_t s_mono_chunk[UPLOAD_CHUNK_BYTES / 2];
static int16_t s_opus_pcm[960]; /* 20 ms at the largest supported Opus rate, 48 kHz. */
static uint8_t s_opus_packet[OPUS_MAX_PACKET_BYTES];

typedef struct {
    bool downmix_stereo16_to_mono;
    bool compress_opus;
    bool compress_adpcm;
    uint32_t sample_rate;
    uint32_t data_offset;
    uint32_t source_data_bytes;
    uint32_t upload_bytes;
    uint32_t sample_count;
    uint16_t source_channels;
} upload_plan_t;

typedef struct {
    FILE *file;
    uint32_t serial;
    uint32_t sequence;
    uint8_t segments[OGG_MAX_SEGMENTS];
    uint8_t payload[OGG_PAGE_PAYLOAD_BYTES];
    int segment_count;
    size_t payload_len;
    uint64_t granule_pos;
} ogg_writer_t;

static ogg_writer_t s_ogg_writer;

static void load_runtime_string(const char *key, const char *fallback,
                                char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%s", fallback ? fallback : "");

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        snprintf(out, out_len, "%s", fallback ? fallback : "");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "runtime config read failed for %s: %s",
                 key, esp_err_to_name(err));
        snprintf(out, out_len, "%s", fallback ? fallback : "");
    }
    nvs_close(h);
}

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
    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t wifi_apply_config(void)
{
    char ssid[33];
    char password[65];
    load_runtime_string(KEY_SSID, CONFIG_OPEN_PLAUD_WIFI_SSID, ssid, sizeof ssid);
    load_runtime_string(KEY_WIFI_PW, CONFIG_OPEN_PLAUD_WIFI_PASSWORD, password, sizeof password);

    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; configure it with BLE companion or menuconfig");
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGW(TAG, "Wi-Fi runtime config is too long");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_cfg = { 0 };
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, password, password_len);
    wifi_cfg.sta.threshold.authmode =
        password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi_config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi_ps");
    ESP_LOGI(TAG, "Wi-Fi configured for SSID \"%s\"", ssid);
    return ESP_OK;
}

static esp_err_t wifi_connect(void)
{
    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once");
    ESP_RETURN_ON_ERROR(wifi_apply_config(), TAG, "wifi_apply_config");

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
    char base[192];
    load_runtime_string(KEY_SERVER, CONFIG_OPEN_PLAUD_SERVER_URL, base, sizeof base);
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

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t ogg_crc_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

static esp_err_t ogg_write_page(FILE *out, uint32_t serial, uint32_t *sequence,
                                uint8_t header_type, uint64_t granule_pos,
                                const uint8_t *segments, int segment_count,
                                const uint8_t *payload, size_t payload_len)
{
    if (segment_count < 0 || segment_count > OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t header[27 + OGG_MAX_SEGMENTS];
    memcpy(header, "OggS", 4);
    header[4] = 0;
    header[5] = header_type;
    put_le64(header + 6, granule_pos);
    put_le32(header + 14, serial);
    put_le32(header + 18, (*sequence)++);
    put_le32(header + 22, 0);
    header[26] = (uint8_t)segment_count;
    if (segment_count > 0) {
        memcpy(header + 27, segments, (size_t)segment_count);
    }

    uint32_t crc = ogg_crc_update(0, header, 27U + (size_t)segment_count);
    crc = ogg_crc_update(crc, payload, payload_len);
    put_le32(header + 22, crc);

    if (fwrite(header, 1, 27U + (size_t)segment_count, out) !=
        27U + (size_t)segment_count) {
        return ESP_FAIL;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, out) != payload_len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ogg_write_single_packet(FILE *out, uint32_t serial,
                                         uint32_t *sequence, uint8_t header_type,
                                         uint64_t granule_pos,
                                         const uint8_t *packet, size_t packet_len)
{
    uint8_t segments[OGG_MAX_SEGMENTS];
    int segment_count = 0;
    size_t remaining = packet_len;
    while (remaining >= 255) {
        if (segment_count >= OGG_MAX_SEGMENTS) {
            return ESP_ERR_INVALID_SIZE;
        }
        segments[segment_count++] = 255;
        remaining -= 255;
    }
    if (segment_count >= OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    segments[segment_count++] = (uint8_t)remaining;

    return ogg_write_page(out, serial, sequence, header_type, granule_pos,
                          segments, segment_count, packet, packet_len);
}

static esp_err_t ogg_flush_audio_page(ogg_writer_t *w, uint8_t header_type)
{
    if (w->segment_count == 0 && header_type == 0) {
        return ESP_OK;
    }
    esp_err_t err = ogg_write_page(w->file, w->serial, &w->sequence, header_type,
                                   w->granule_pos, w->segments, w->segment_count,
                                   w->payload, w->payload_len);
    w->segment_count = 0;
    w->payload_len = 0;
    return err;
}

static int opus_lacing_segments(size_t packet_len)
{
    return (int)(packet_len / 255) + 1;
}

static esp_err_t ogg_queue_audio_packet(ogg_writer_t *w, const uint8_t *packet,
                                        size_t packet_len, uint64_t granule_pos)
{
    int need_segments = opus_lacing_segments(packet_len);
    if (need_segments > OGG_MAX_SEGMENTS || packet_len > OGG_PAGE_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (w->segment_count + need_segments > OGG_MAX_SEGMENTS ||
        w->payload_len + packet_len > OGG_PAGE_PAYLOAD_BYTES) {
        esp_err_t err = ogg_flush_audio_page(w, 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    size_t remaining = packet_len;
    while (remaining >= 255) {
        w->segments[w->segment_count++] = 255;
        remaining -= 255;
    }
    w->segments[w->segment_count++] = (uint8_t)remaining;
    memcpy(w->payload + w->payload_len, packet, packet_len);
    w->payload_len += packet_len;
    w->granule_pos = granule_pos;
    return ESP_OK;
}

static esp_err_t ogg_write_opus_headers(FILE *out, uint32_t serial,
                                        uint32_t *sequence,
                                        uint32_t input_sample_rate)
{
    uint8_t head[19] = { 0 };
    memcpy(head, "OpusHead", 8);
    head[8] = 1;        /* version */
    head[9] = 1;        /* mono */
    put_le16(head + 10, 0);
    put_le32(head + 12, input_sample_rate);
    put_le16(head + 16, 0);
    head[18] = 0;       /* no channel mapping table for mono/stereo */
    esp_err_t err = ogg_write_single_packet(out, serial, sequence, 0x02, 0, head, sizeof head);
    if (err != ESP_OK) {
        return err;
    }

    const char vendor[] = "open-plaud";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le32(tags + 8, sizeof(vendor) - 1);
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_le32(tags + 12 + sizeof(vendor) - 1, 0);
    return ogg_write_single_packet(out, serial, sequence, 0, 0, tags, sizeof tags);
}

static void write_pcm_wav_header(uint8_t *h, uint32_t sample_rate,
                                 uint16_t channels, uint16_t bits,
                                 uint32_t data_bytes)
{
    const uint32_t byte_rate = sample_rate * channels * (bits / 8);
    const uint16_t block_align = channels * (bits / 8);

    memcpy(h + 0, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, channels);
    put_le32(h + 24, sample_rate);
    put_le32(h + 28, byte_rate);
    put_le16(h + 32, block_align);
    put_le16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

static void write_opad_header(uint8_t *h, const upload_plan_t *plan, int16_t predictor)
{
    memcpy(h + 0, "OPADPCM1", 8);
    put_le32(h + 8, plan->sample_rate);
    put_le16(h + 12, 1);              /* decoded channels */
    put_le32(h + 14, plan->sample_count);
    put_le16(h + 18, (uint16_t)predictor);
    h[20] = 0;                        /* initial IMA index */
    h[21] = 0;                        /* reserved */
}

static const int s_adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int s_adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static uint8_t adpcm_encode_sample(int16_t sample, int *predictor, int *index)
{
    int step = s_adpcm_step_table[*index];
    int diff = (int)sample - *predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    int delta = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        delta += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        diff -= step >> 1;
        delta += step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        delta += step >> 2;
    }

    if (code & 8) {
        *predictor -= delta;
    } else {
        *predictor += delta;
    }
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;

    *index += s_adpcm_index_table[code & 0x0f];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return code & 0x0f;
}

static upload_plan_t make_upload_plan(FILE *file, long file_size)
{
    upload_plan_t plan = {
        .downmix_stereo16_to_mono = false,
        .compress_opus = false,
        .compress_adpcm = false,
        .sample_rate = 0,
        .data_offset = 0,
        .source_data_bytes = file_size > 0 && file_size <= UINT32_MAX ? (uint32_t)file_size : 0,
        .upload_bytes = file_size > 0 && file_size <= UINT32_MAX ? (uint32_t)file_size : 0,
        .sample_count = 0,
        .source_channels = 0,
    };

    if (file_size < WAV_HEADER_BYTES || file_size > UINT32_MAX) {
        return plan;
    }

    uint8_t h[WAV_HEADER_BYTES];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(h, 1, sizeof h, file) != sizeof h) {
        return plan;
    }

    const bool canonical_pcm =
        memcmp(h + 0, "RIFF", 4) == 0 &&
        memcmp(h + 8, "WAVE", 4) == 0 &&
        memcmp(h + 12, "fmt ", 4) == 0 &&
        memcmp(h + 36, "data", 4) == 0 &&
        le32(h + 16) == 16 &&
        le16(h + 20) == 1;
    if (!canonical_pcm) {
        return plan;
    }

    const uint16_t channels = le16(h + 22);
    const uint32_t sample_rate = le32(h + 24);
    const uint16_t bits = le16(h + 34);
    const uint32_t data_bytes = le32(h + 40);

    if ((channels == 1 || channels == 2) && bits == 16 &&
        data_bytes >= (uint32_t)(channels * 2) &&
        (uint64_t)WAV_HEADER_BYTES + data_bytes <= (uint64_t)file_size) {
        const uint32_t frames = data_bytes / ((uint32_t)channels * 2);
        plan.sample_rate = sample_rate;
        plan.data_offset = WAV_HEADER_BYTES;
        plan.source_data_bytes = frames * (uint32_t)channels * 2;
        plan.sample_count = frames;
        plan.source_channels = channels;

#if CONFIG_OPEN_PLAUD_COMPRESS_TRANSFERS
#if CONFIG_OPEN_PLAUD_OPUS_TRANSFERS
        plan.compress_opus =
            sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
            sample_rate == 24000 || sample_rate == 48000;
#endif
        plan.compress_adpcm = true;
        plan.downmix_stereo16_to_mono = channels == 2;
        plan.upload_bytes = OPAD_HEADER_BYTES + (frames / 2);
        return plan;
#else
        if (channels == 1) {
            return plan;
        }
        const uint32_t mono_data_bytes = frames * 2;
        plan.downmix_stereo16_to_mono = true;
        plan.upload_bytes = WAV_HEADER_BYTES + mono_data_bytes;
#endif
    }
    return plan;
}

static esp_err_t make_temp_opus_path(const char *path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s.opus.tmp", path);
    return (n < 0 || (size_t)n >= out_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t make_opus_upload_filename(const char *path, char *out, size_t out_len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    int stem_len = dot && dot > base ? (int)(dot - base) : (int)strlen(base);
    int n = snprintf(out, out_len, "%.*s.opus", stem_len, base);
    return (n < 0 || (size_t)n >= out_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t encode_file_ogg_opus(FILE *file, const upload_plan_t *plan,
                                      const char *src_path, char *tmp_path,
                                      size_t tmp_path_len, long *out_size)
{
    ESP_RETURN_ON_FALSE(plan && plan->compress_opus, ESP_ERR_INVALID_ARG, TAG, "opus plan");
    ESP_RETURN_ON_ERROR(make_temp_opus_path(src_path, tmp_path, tmp_path_len),
                        TAG, "opus temp path");
    if (fseek(file, (long)plan->data_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        ESP_LOGW(TAG, "cannot create temporary Opus file %s", tmp_path);
        return ESP_FAIL;
    }

    int opus_err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create((opus_int32)plan->sample_rate, 1,
                                           OPUS_APPLICATION_VOIP, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        if (enc) {
            opus_encoder_destroy(enc);
        }
        fclose(out);
        remove(tmp_path);
        ESP_LOGW(TAG, "Opus encoder create failed: %d", opus_err);
        return ESP_FAIL;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(CONFIG_OPEN_PLAUD_OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    uint32_t sequence = 0;
    const uint32_t serial = esp_random();
    esp_err_t err = ogg_write_opus_headers(out, serial, &sequence, plan->sample_rate);
    memset(&s_ogg_writer, 0, sizeof(s_ogg_writer));
    s_ogg_writer.file = out;
    s_ogg_writer.serial = serial;
    s_ogg_writer.sequence = sequence;

    const uint32_t frame_samples = plan->sample_rate / (1000 / OPUS_FRAME_MS);
    const uint32_t frame_bytes = (uint32_t)plan->source_channels * 2;
    uint32_t remaining_samples = plan->sample_count;
    uint32_t processed_samples = 0;

    while (err == ESP_OK && remaining_samples > 0) {
        uint32_t samples_this_frame = remaining_samples < frame_samples ?
                                      remaining_samples : frame_samples;
        size_t read_bytes = (size_t)samples_this_frame * frame_bytes;
        if (read_bytes > sizeof(s_upload_chunk)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        size_t n = fread(s_upload_chunk, 1, read_bytes, file);
        if (n != read_bytes) {
            err = ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            break;
        }

        for (uint32_t i = 0; i < samples_this_frame; i++) {
            const uint8_t *frame = s_upload_chunk + (i * frame_bytes);
            if (plan->source_channels == 1) {
                s_opus_pcm[i] = (int16_t)le16(frame);
            } else {
                const int16_t left = (int16_t)le16(frame);
                const int16_t right = (int16_t)le16(frame + 2);
                s_opus_pcm[i] = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            }
        }
        if (samples_this_frame < frame_samples) {
            memset(s_opus_pcm + samples_this_frame, 0,
                   (frame_samples - samples_this_frame) * sizeof(s_opus_pcm[0]));
        }

        int packet_len = opus_encode(enc, s_opus_pcm, (int)frame_samples,
                                     s_opus_packet, sizeof(s_opus_packet));
        if (packet_len < 0) {
            ESP_LOGW(TAG, "Opus encode failed: %d", packet_len);
            err = ESP_FAIL;
            break;
        }
        processed_samples += samples_this_frame;
        remaining_samples -= samples_this_frame;
        uint64_t granule = ((uint64_t)processed_samples * 48000U) / plan->sample_rate;
        err = ogg_queue_audio_packet(&s_ogg_writer, s_opus_packet, (size_t)packet_len, granule);
    }

    if (err == ESP_OK) {
        err = ogg_flush_audio_page(&s_ogg_writer, 0x04);
    }
    opus_encoder_destroy(enc);

    if (err == ESP_OK && fflush(out) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fseek(out, 0, SEEK_END) == 0) {
        long size = ftell(out);
        if (size > 0) {
            *out_size = size;
        } else {
            err = ESP_FAIL;
        }
    } else if (err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (fclose(out) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        remove(tmp_path);
    }
    return err;
}

static esp_err_t upload_file_passthrough(esp_http_client_handle_t client, FILE *file,
                                         const char *filename, const char *path,
                                         uint32_t total_bytes,
                                         net_upload_progress_fn progress, void *ctx)
{
    long sent = 0;
    int last_logged_pct = -5;
    while (true) {
        size_t n = fread(s_upload_chunk, 1, sizeof(s_upload_chunk), file);
        if (n > 0) {
            esp_err_t err = checked_write(client, (const char *)s_upload_chunk, (int)n);
            if (err != ESP_OK) {
                return err;
            }
            sent += (long)n;
            if (progress && total_bytes <= INT_MAX) {
                progress(path, (int)sent, (int)total_bytes,
                         NET_UPLOAD_PHASE_UPLOADING, ctx);
            }
            int pct = total_bytes > 0 ? (int)(sent * 100 / total_bytes) : 100;
            if (pct >= 100 || pct - last_logged_pct >= 10) {
                ESP_LOGI(TAG, "upload progress %s: %d%% (%ld/%u bytes)",
                         filename, pct, sent, (unsigned)total_bytes);
                last_logged_pct = pct;
            }
        }
        if (n < sizeof(s_upload_chunk)) {
            if (ferror(file)) {
                return ESP_FAIL;
            }
            return ESP_OK;
        }
    }
}

static esp_err_t upload_file_downmix_mono(esp_http_client_handle_t client, FILE *file,
                                          const upload_plan_t *plan,
                                          const char *filename, const char *path,
                                          net_upload_progress_fn progress, void *ctx)
{
    uint8_t wav_header[WAV_HEADER_BYTES];
    const uint32_t mono_data_bytes = plan->upload_bytes - WAV_HEADER_BYTES;
    write_pcm_wav_header(wav_header, plan->sample_rate, 1, 16, mono_data_bytes);

    esp_err_t err = checked_write(client, (const char *)wav_header, sizeof wav_header);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t source_remaining = plan->source_data_bytes;
    uint32_t sent = WAV_HEADER_BYTES;
    int last_logged_pct = -5;
    if (progress && plan->upload_bytes <= INT_MAX) {
        progress(path, (int)sent, (int)plan->upload_bytes,
                 NET_UPLOAD_PHASE_UPLOADING, ctx);
    }

    while (source_remaining > 0) {
        size_t to_read = sizeof(s_upload_chunk);
        if (to_read > source_remaining) {
            to_read = source_remaining;
        }
        to_read -= to_read % 4;
        if (to_read == 0) {
            break;
        }

        size_t n = fread(s_upload_chunk, 1, to_read, file);
        if (n == 0) {
            return ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        }
        n -= n % 4;
        const size_t frames = n / 4;
        for (size_t i = 0; i < frames; i++) {
            const int16_t left = (int16_t)le16(s_upload_chunk + (i * 4));
            const int16_t right = (int16_t)le16(s_upload_chunk + (i * 4) + 2);
            const int16_t mono = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            put_le16(s_mono_chunk + (i * 2), (uint16_t)mono);
        }

        const int mono_bytes = (int)(frames * 2);
        err = checked_write(client, (const char *)s_mono_chunk, mono_bytes);
        if (err != ESP_OK) {
            return err;
        }

        source_remaining -= (uint32_t)n;
        sent += (uint32_t)mono_bytes;
        if (progress && plan->upload_bytes <= INT_MAX) {
            progress(path, (int)sent, (int)plan->upload_bytes,
                     NET_UPLOAD_PHASE_UPLOADING, ctx);
        }
        int pct = plan->upload_bytes > 0 ? (int)((uint64_t)sent * 100 / plan->upload_bytes) : 100;
        if (pct >= 100 || pct - last_logged_pct >= 10) {
            ESP_LOGI(TAG, "upload progress %s: %d%% (%u/%u bytes)",
                     filename, pct, (unsigned)sent, (unsigned)plan->upload_bytes);
            last_logged_pct = pct;
        }
    }

    return ESP_OK;
}

static esp_err_t upload_file_adpcm(esp_http_client_handle_t client, FILE *file,
                                   const upload_plan_t *plan,
                                   const char *filename, const char *path,
                                   net_upload_progress_fn progress, void *ctx)
{
    const uint32_t frame_bytes = (uint32_t)plan->source_channels * 2;
    uint8_t first_frame[4] = { 0 };
    if (plan->source_data_bytes < frame_bytes ||
        fread(first_frame, 1, frame_bytes, file) != frame_bytes) {
        return ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
    }

    int16_t first_sample = 0;
    if (plan->source_channels == 1) {
        first_sample = (int16_t)le16(first_frame);
    } else {
        const int16_t left = (int16_t)le16(first_frame);
        const int16_t right = (int16_t)le16(first_frame + 2);
        first_sample = (int16_t)(((int32_t)left + (int32_t)right) / 2);
    }

    uint8_t header[OPAD_HEADER_BYTES];
    write_opad_header(header, plan, first_sample);
    esp_err_t err = checked_write(client, (const char *)header, sizeof header);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t source_remaining = plan->source_data_bytes - frame_bytes;
    uint32_t sent = OPAD_HEADER_BYTES;
    int predictor = first_sample;
    int index = 0;
    int last_logged_pct = -5;
    bool have_low_nibble = false;
    uint8_t pending_byte = 0;

    if (progress && plan->upload_bytes <= INT_MAX) {
        progress(path, (int)sent, (int)plan->upload_bytes,
                 NET_UPLOAD_PHASE_UPLOADING, ctx);
    }

    while (source_remaining > 0) {
        size_t to_read = sizeof(s_upload_chunk);
        if (to_read > source_remaining) {
            to_read = source_remaining;
        }
        to_read -= to_read % frame_bytes;
        if (to_read == 0) {
            break;
        }

        size_t n = fread(s_upload_chunk, 1, to_read, file);
        if (n == 0) {
            return ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        }
        n -= n % frame_bytes;

        size_t out_n = 0;
        const size_t frames = n / frame_bytes;
        for (size_t i = 0; i < frames; i++) {
            const uint8_t *frame = s_upload_chunk + (i * frame_bytes);
            int16_t mono = 0;
            if (plan->source_channels == 1) {
                mono = (int16_t)le16(frame);
            } else {
                const int16_t left = (int16_t)le16(frame);
                const int16_t right = (int16_t)le16(frame + 2);
                mono = (int16_t)(((int32_t)left + (int32_t)right) / 2);
            }

            const uint8_t nibble = adpcm_encode_sample(mono, &predictor, &index);
            if (!have_low_nibble) {
                pending_byte = nibble;
                have_low_nibble = true;
            } else {
                s_mono_chunk[out_n++] = pending_byte | (uint8_t)(nibble << 4);
                have_low_nibble = false;
            }
        }

        if (out_n > 0) {
            err = checked_write(client, (const char *)s_mono_chunk, (int)out_n);
            if (err != ESP_OK) {
                return err;
            }
            sent += (uint32_t)out_n;
        }

        source_remaining -= (uint32_t)n;
        if (progress && plan->upload_bytes <= INT_MAX) {
            progress(path, (int)sent, (int)plan->upload_bytes,
                     NET_UPLOAD_PHASE_UPLOADING, ctx);
        }
        int pct = plan->upload_bytes > 0 ? (int)((uint64_t)sent * 100 / plan->upload_bytes) : 100;
        if (pct >= 100 || pct - last_logged_pct >= 10) {
            ESP_LOGI(TAG, "upload progress %s: %d%% (%u/%u compressed bytes)",
                     filename, pct, (unsigned)sent, (unsigned)plan->upload_bytes);
            last_logged_pct = pct;
        }
    }

    if (have_low_nibble) {
        err = checked_write(client, (const char *)&pending_byte, 1);
        if (err != ESP_OK) {
            return err;
        }
        sent++;
    }

    if (progress && plan->upload_bytes <= INT_MAX) {
        progress(path, (int)sent, (int)plan->upload_bytes,
                 NET_UPLOAD_PHASE_UPLOADING, ctx);
    }
    ESP_LOGI(TAG, "upload progress %s: 100%% (%u/%u compressed bytes)",
             filename, (unsigned)sent, (unsigned)plan->upload_bytes);
    return sent == plan->upload_bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t net_upload_wav(const char *path, net_upload_progress_fn progress, void *ctx)
{
    ESP_RETURN_ON_FALSE(path, ESP_ERR_INVALID_ARG, TAG, "path is NULL");
    if (progress) {
        progress(path, 0, 0, NET_UPLOAD_PHASE_WIFI_CONNECTING, ctx);
    }
    esp_err_t err = wifi_connect();
    if (err != ESP_OK) {
        if (progress) {
            progress(path, 0, 0, NET_UPLOAD_PHASE_WIFI_FAILED, ctx);
        }
        ESP_RETURN_ON_ERROR(err, TAG, "wifi_connect");
    }
    if (progress) {
        progress(path, 0, 0, NET_UPLOAD_PHASE_WIFI_CONNECTED, ctx);
    }

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
    upload_plan_t plan = make_upload_plan(file, file_size);

    char tmp_opus_path[96] = { 0 };
    char opus_filename[40] = { 0 };
    bool uploading_temp_opus = false;
    uint32_t upload_bytes = plan.upload_bytes;

    if (plan.compress_opus) {
        long opus_size = 0;
        ESP_LOGI(TAG, "encoding %s to Ogg Opus at %d bps",
                 path, CONFIG_OPEN_PLAUD_OPUS_BITRATE);
        err = encode_file_ogg_opus(file, &plan, path, tmp_opus_path,
                                   sizeof(tmp_opus_path), &opus_size);
        if (err == ESP_OK && opus_size > 0 && opus_size <= UINT32_MAX &&
            make_opus_upload_filename(path, opus_filename, sizeof(opus_filename)) == ESP_OK) {
            fclose(file);
            file = fopen(tmp_opus_path, "rb");
            if (!file) {
                remove(tmp_opus_path);
                return ESP_FAIL;
            }
            uploading_temp_opus = true;
            plan.compress_adpcm = false;
            plan.downmix_stereo16_to_mono = false;
            upload_bytes = (uint32_t)opus_size;
        } else {
            ESP_LOGW(TAG, "Opus encode failed; falling back to ADPCM transfer");
            plan.compress_opus = false;
            err = ESP_OK;
        }
    }

    if (!uploading_temp_opus &&
        fseek(file, (plan.downmix_stereo16_to_mono || plan.compress_adpcm) ?
              (long)plan.data_offset : 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    char url[256];
    err = build_upload_url(url, sizeof(url));
    if (err != ESP_OK) {
        fclose(file);
        if (uploading_temp_opus) {
            remove(tmp_opus_path);
        }
        ESP_LOGE(TAG, "server URL is too long");
        return err;
    }

    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    const char *upload_filename = uploading_temp_opus ? opus_filename : filename;
    const char *part_content_type = uploading_temp_opus ? OGG_OPUS_MEDIA_TYPE :
                                    (plan.compress_adpcm ? OPAD_MEDIA_TYPE : "audio/wav");
    const char *boundary = "open-plaud-esp32s3";
    char head[256];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, upload_filename, part_content_type);
    const char *tail_fmt = "\r\n--%s--\r\n";
    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail), tail_fmt, boundary);
    if (head_len < 0 || head_len >= (int)sizeof(head) ||
        tail_len < 0 || tail_len >= (int)sizeof(tail)) {
        fclose(file);
        if (uploading_temp_opus) {
            remove(tmp_opus_path);
        }
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
        if (uploading_temp_opus) {
            remove(tmp_opus_path);
        }
        return ESP_FAIL;
    }

    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // Optional shared-secret auth: matches the server's OP_AUTH_TOKEN. Empty by
    // default (open server); BLE/NVS config overrides the menuconfig value.
    char token[161];
    load_runtime_string(KEY_TOKEN, CONFIG_OPEN_PLAUD_AUTH_TOKEN, token, sizeof token);
    if (strlen(token) > 0) {
        char auth[176];
        int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", token);
        if (auth_len > 0 && auth_len < (int)sizeof(auth)) {
            esp_http_client_set_header(client, "Authorization", auth);
        }
    }

    int64_t total_len = (int64_t)head_len + upload_bytes + tail_len;
    if (total_len > INT_MAX) {
        fclose(file);
        if (uploading_temp_opus) {
            remove(tmp_opus_path);
        }
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "file is too large for esp_http_client_open content length");
        return ESP_ERR_INVALID_SIZE;
    }
    int content_len = (int)total_len;

    ESP_LOGI(TAG, "uploading %s (%ld bytes%s%u bytes) -> %s",
             path, file_size,
             uploading_temp_opus ? ", Ogg Opus stream " :
             plan.compress_adpcm ? ", compressed stream " :
             (plan.downmix_stereo16_to_mono ? ", mono stream " : ", stream "),
             (unsigned)upload_bytes, url);
    if (uploading_temp_opus) {
        ESP_LOGI(TAG, "uploading %s as %s", filename, upload_filename);
    } else if (plan.compress_adpcm) {
        ESP_LOGI(TAG, "compressing %s to Open Plaud ADPCM for transfer", filename);
    } else if (plan.downmix_stereo16_to_mono) {
        ESP_LOGI(TAG, "downmixing %s from stereo 16-bit PCM to mono for upload", filename);
    }
    if (progress && upload_bytes > 0 && upload_bytes <= INT_MAX) {
        progress(path, 0, (int)upload_bytes, NET_UPLOAD_PHASE_UPLOADING, ctx);
    }
    err = esp_http_client_open(client, content_len);
    if (err == ESP_OK) {
        err = checked_write(client, head, head_len);
    }

    if (err == ESP_OK) {
        if (uploading_temp_opus) {
            err = upload_file_passthrough(client, file, upload_filename, path,
                                          upload_bytes, progress, ctx);
        } else if (plan.compress_adpcm) {
            err = upload_file_adpcm(client, file, &plan, filename, path, progress, ctx);
        } else if (plan.downmix_stereo16_to_mono) {
            err = upload_file_downmix_mono(client, file, &plan, filename, path, progress, ctx);
        } else {
            err = upload_file_passthrough(client, file, filename, path, plan.upload_bytes, progress, ctx);
        }
    }

    if (err == ESP_OK) {
        err = checked_write(client, tail, tail_len);
    }
    fclose(file);
    if (uploading_temp_opus) {
        remove(tmp_opus_path);
    }

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
