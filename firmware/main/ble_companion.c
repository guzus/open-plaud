#include "ble_companion.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "rec_store.h"
#include "sdcard.h"

#if CONFIG_OPEN_PLAUD_BLE_COMPANION
#include "ble_audio.h"
#include "cJSON.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);
#endif

#if !CONFIG_OPEN_PLAUD_BLE_COMPANION
esp_err_t open_plaud_ble_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void open_plaud_ble_set_status(const open_plaud_ble_status_t *status)
{
    (void)status;
}

bool open_plaud_ble_take_command(open_plaud_ble_command_t *out)
{
    if (out) {
        *out = OPEN_PLAUD_BLE_COMMAND_NONE;
    }
    return false;
}
#else

static const char *TAG = "ble_companion";

#define CFG_NS              "op_ble_cfg"
#define KEY_SSID            "ssid"
#define KEY_WIFI_PW         "wifi_pw"
#define KEY_SERVER          "server"
#define KEY_TOKEN           "token"

#define JSON_WRITE_MAX      512
#define JSON_READ_MAX       384
#define CLIP_PATH_MAX       40
#define BLE_CHUNK_VALUE_MAX 180
#define BLE_CHUNK_HDR_LEN   16
#define BLE_CHUNK_PAYLOAD   (BLE_CHUNK_VALUE_MAX - BLE_CHUNK_HDR_LEN)
#define BLE_PCM_MAX_SAMPLES ((BLE_CHUNK_PAYLOAD - 4) * 2)

typedef enum {
    CHR_STATUS = 1,
    CHR_CONFIG,
    CHR_COMMAND,
    CHR_TRANSFER_CONTROL,
    CHR_TRANSFER_CHUNK,
} chr_id_t;

typedef struct {
    char path[CLIP_PATH_MAX];
    uint32_t offset;
    bool adpcm;
} transfer_cursor_t;

typedef struct {
    bool valid;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint16_t channels;
    uint16_t bits;
} wav_info_t;

static uint16_t s_status_handle;
static bool s_started;
static uint8_t s_own_addr_type;
static open_plaud_ble_status_t s_status = {
    .sync_done = -1,
    .sync_total = -1,
    .upload_pct = -1,
    .wifi_connected = false,
};
static open_plaud_ble_command_t s_pending_command = OPEN_PLAUD_BLE_COMMAND_NONE;
static transfer_cursor_t s_cursor = {
    .path = "",
    .offset = 0,
    .adpcm = true,
};

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                     0x01, 0x00, 0x44, 0x55, 0x41, 0x4c, 0x50, 0x6f);
static const ble_uuid128_t s_command_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                     0x02, 0x00, 0x44, 0x55, 0x41, 0x4c, 0x50, 0x6f);
static const ble_uuid128_t s_status_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                     0x03, 0x00, 0x44, 0x55, 0x41, 0x4c, 0x50, 0x6f);
static const ble_uuid128_t s_transfer_control_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                     0x04, 0x00, 0x44, 0x55, 0x41, 0x4c, 0x50, 0x6f);
static const ble_uuid128_t s_transfer_chunk_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                     0x05, 0x00, 0x44, 0x55, 0x41, 0x4c, 0x50, 0x6f);

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_status_uuid.u,
                .access_cb = access_cb,
                .arg = (void *)CHR_STATUS,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {
                .uuid = &s_command_uuid.u,
                .access_cb = access_cb,
                .arg = (void *)CHR_COMMAND,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_transfer_control_uuid.u,
                .access_cb = access_cb,
                .arg = (void *)CHR_TRANSFER_CONTROL,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_transfer_chunk_uuid.u,
                .access_cb = access_cb,
                .arg = (void *)CHR_TRANSFER_CHUNK,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

static void op_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void op_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

static int append_bytes(struct os_mbuf *om, const void *data, size_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int append_str(struct os_mbuf *om, const char *s)
{
    return append_bytes(om, s, strlen(s));
}

static bool parse_rec_name(const char *name, unsigned *out_num)
{
    if (strlen(name) != 12 || strncasecmp(name, "rec_", 4) != 0 ||
        strcasecmp(name + 8, ".wav") != 0) {
        return false;
    }
    unsigned num = 0;
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        num = num * 10 + (unsigned)(name[i] - '0');
    }
    *out_num = num;
    return true;
}

static bool first_pending_clip(char *out, size_t out_len)
{
    DIR *d = opendir(SDCARD_MOUNT_POINT);
    if (!d) {
        return false;
    }

    bool found = false;
    unsigned best = 0;
    unsigned num;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (parse_rec_name(ent->d_name, &num) && (!found || num < best)) {
            found = true;
            best = num;
        }
    }
    closedir(d);
    if (!found) {
        return false;
    }
    return snprintf(out, out_len, SDCARD_MOUNT_POINT "/rec_%04u.wav", best) < (int)out_len;
}

static esp_err_t nvs_get_str_flag(nvs_handle_t h, const char *key,
                                  char *out, size_t out_len, bool *present)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (out_len > 0) out[0] = '\0';
        *present = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *present = true;
    return ESP_OK;
}

static esp_err_t nvs_set_json_string(nvs_handle_t h, cJSON *root,
                                     const char *json_key, const char *nvs_key,
                                     size_t max_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, json_key);
    if (!item) {
        return ESP_OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(item->valuestring);
    if (len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) {
        esp_err_t err = nvs_erase_key(h, nvs_key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_str(h, nvs_key, item->valuestring);
}

static int write_config(struct os_mbuf *om)
{
    char buf[JSON_WRITE_MAX];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, buf, sizeof(buf) - 1, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) err = nvs_set_json_string(h, root, "ssid", KEY_SSID, 32);
    if (err == ESP_OK) err = nvs_set_json_string(h, root, "password", KEY_WIFI_PW, 64);
    if (err == ESP_OK) err = nvs_set_json_string(h, root, "server_url", KEY_SERVER, 160);
    if (err == ESP_OK) err = nvs_set_json_string(h, root, "auth_token", KEY_TOKEN, 160);
    if (err == ESP_OK) err = nvs_commit(h);
    if (h) nvs_close(h);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config write failed: %s", esp_err_to_name(err));
        return err == ESP_ERR_INVALID_SIZE ? BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN
                                           : BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGI(TAG, "BLE companion config updated");
    return 0;
}

static int read_config(struct os_mbuf *om)
{
    char ssid[33];
    char server[161];
    char tmp[2];
    bool ssid_set = false, server_set = false, password_set = false, token_set = false;

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        (void)nvs_get_str_flag(h, KEY_SSID, ssid, sizeof ssid, &ssid_set);
        (void)nvs_get_str_flag(h, KEY_SERVER, server, sizeof server, &server_set);
        (void)nvs_get_str_flag(h, KEY_WIFI_PW, tmp, sizeof tmp, &password_set);
        (void)nvs_get_str_flag(h, KEY_TOKEN, tmp, sizeof tmp, &token_set);
        nvs_close(h);
    } else {
        ssid[0] = '\0';
        server[0] = '\0';
    }

    char json[JSON_READ_MAX];
    snprintf(json, sizeof json,
             "{\"ssid\":\"%s\",\"ssid_set\":%s,\"password_set\":%s,"
             "\"server_url\":\"%s\",\"server_set\":%s,\"auth_token_set\":%s}",
             ssid_set ? ssid : "", ssid_set ? "true" : "false",
             password_set ? "true" : "false",
             server_set ? server : "", server_set ? "true" : "false",
             token_set ? "true" : "false");
    return append_str(om, json);
}

static int read_status(struct os_mbuf *om)
{
    const char *state = "idle";
    if (s_status.sync_failed) {
        state = "failed";
    } else if (s_status.sync_total >= 0) {
        state = s_status.sync_done >= s_status.sync_total ? "complete" : "syncing";
    }

    char ssid[33] = "";
    bool ssid_set = false;
    nvs_handle_t h = 0;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_str_flag(h, KEY_SSID, ssid, sizeof ssid, &ssid_set);
        nvs_close(h);
    }
    if (!ssid_set && strlen(CONFIG_OPEN_PLAUD_WIFI_SSID) > 0) {
        snprintf(ssid, sizeof ssid, "%s", CONFIG_OPEN_PLAUD_WIFI_SSID);
        ssid_set = true;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceName", "Open Plaud");
    cJSON_AddNumberToObject(root, "protocolVersion", 1);
    cJSON_AddStringToObject(root, "syncState", state);
    cJSON_AddNumberToObject(root, "queuedClips", rec_store_pending_count());
    cJSON_AddBoolToObject(root, "isCharging", false);
    cJSON_AddStringToObject(root, "message",
                            s_status.sync_failed ? "Sync stopped before the queue drained." :
                            (s_status.sync_total >= 0 ? "Sync status updated." :
                             "Ready."));
    if (ssid_set) {
        cJSON_AddStringToObject(root, "wifiSSID", ssid);
    }
    if (s_status.upload_pct >= 0) {
        cJSON_AddNumberToObject(root, "transferPercent", s_status.upload_pct);
    }
    cJSON *pref = cJSON_AddObjectToObject(root, "transferPreference");
    cJSON_AddStringToObject(pref, "route", "wifi");
    cJSON_AddStringToObject(pref, "compression",
                            s_cursor.adpcm ? "ima_adpcm" : "mono_wav");

    cJSON *features = cJSON_AddArrayToObject(root, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("wifi-config"));
    cJSON_AddItemToArray(features, cJSON_CreateString("queue-status"));
    cJSON_AddItemToArray(features, cJSON_CreateString("command-sync"));
    cJSON_AddItemToArray(features, cJSON_CreateString("clip-chunk-adpcm-v1"));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    int rc = append_str(om, json);
    cJSON_free(json);
    return rc;
}

static int write_command(struct os_mbuf *om)
{
    char buf[JSON_WRITE_MAX];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, buf, sizeof(buf) - 1, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    cJSON *op = root ? cJSON_GetObjectItemCaseSensitive(root, "op") : NULL;
    if (!root || !cJSON_IsString(op) || !op->valuestring) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (strcmp(op->valuestring, "request_status") == 0) {
        /* A read/notify of the status characteristic carries the state. */
    } else if (strcmp(op->valuestring, "start_wifi_sync") == 0 ||
               strcmp(op->valuestring, "sync") == 0) {
        s_pending_command = OPEN_PLAUD_BLE_COMMAND_SYNC;
    } else if (strcmp(op->valuestring, "stop_sync") == 0) {
        s_pending_command = OPEN_PLAUD_BLE_COMMAND_NONE;
    } else if (strcmp(op->valuestring, "configure_wifi") == 0 ||
               strcmp(op->valuestring, "configure_server") == 0) {
        nvs_handle_t h = 0;
        esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
        if (strcmp(op->valuestring, "configure_wifi") == 0) {
            if (err == ESP_OK) err = nvs_set_json_string(h, root, "ssid", KEY_SSID, 32);
            if (err == ESP_OK) err = nvs_set_json_string(h, root, "password", KEY_WIFI_PW, 64);
        } else {
            if (err == ESP_OK) err = nvs_set_json_string(h, root, "url", KEY_SERVER, 160);
            if (err == ESP_OK) err = nvs_set_json_string(h, root, "auth_token", KEY_TOKEN, 160);
        }
        if (err == ESP_OK) err = nvs_commit(h);
        if (h) nvs_close(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "command config write failed: %s", esp_err_to_name(err));
            cJSON_Delete(root);
            return err == ESP_ERR_INVALID_SIZE ? BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN
                                               : BLE_ATT_ERR_UNLIKELY;
        }
        ESP_LOGI(TAG, "BLE companion %s applied", op->valuestring);
    } else if (strcmp(op->valuestring, "set_transfer_preference") == 0) {
        cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
        cJSON *codec = cJSON_GetObjectItemCaseSensitive(root, "compression");
        if (route && cJSON_IsString(route) && codec && cJSON_IsString(codec)) {
            s_cursor.adpcm = strcmp(codec->valuestring, "mono_wav") != 0;
        }
    } else {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }
    cJSON_Delete(root);
    return 0;
}

static int read_command(struct os_mbuf *om)
{
    char json[48];
    snprintf(json, sizeof json, "{\"pending\":\"%s\"}",
             s_pending_command == OPEN_PLAUD_BLE_COMMAND_SYNC ? "sync" : "none");
    return append_str(om, json);
}

static int write_transfer_control(struct os_mbuf *om)
{
    char buf[JSON_WRITE_MAX];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, buf, sizeof(buf) - 1, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }

    cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (path && cJSON_IsString(path) && path->valuestring) {
        if (strncmp(path->valuestring, SDCARD_MOUNT_POINT "/rec_", strlen(SDCARD_MOUNT_POINT "/rec_")) != 0 ||
            strlen(path->valuestring) >= sizeof(s_cursor.path)) {
            cJSON_Delete(root);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        snprintf(s_cursor.path, sizeof s_cursor.path, "%s", path->valuestring);
    } else if (s_cursor.path[0] == '\0') {
        (void)first_pending_clip(s_cursor.path, sizeof s_cursor.path);
    }

    cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
    if (offset && cJSON_IsNumber(offset) && offset->valuedouble >= 0) {
        s_cursor.offset = (uint32_t)offset->valuedouble;
    }
    cJSON *codec = cJSON_GetObjectItemCaseSensitive(root, "codec");
    if (codec && cJSON_IsString(codec) && codec->valuestring) {
        s_cursor.adpcm = strcmp(codec->valuestring, "raw") != 0;
    }
    cJSON_Delete(root);
    return 0;
}

static int read_transfer_control(struct os_mbuf *om)
{
    if (s_cursor.path[0] == '\0') {
        (void)first_pending_clip(s_cursor.path, sizeof s_cursor.path);
    }
    char json[JSON_READ_MAX];
    snprintf(json, sizeof json, "{\"path\":\"%s\",\"offset\":%u,\"codec\":\"%s\"}",
             s_cursor.path, (unsigned)s_cursor.offset,
             s_cursor.adpcm ? "ima-adpcm" : "raw");
    return append_str(om, json);
}

static wav_info_t read_wav_info(FILE *file)
{
    wav_info_t info = { 0 };
    uint8_t h[44];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(h, 1, sizeof h, file) != sizeof h) {
        return info;
    }
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0 ||
        memcmp(h + 12, "fmt ", 4) != 0 || memcmp(h + 36, "data", 4) != 0 ||
        le32(h + 16) != 16 || le16(h + 20) != 1) {
        return info;
    }
    info.channels = le16(h + 22);
    info.bits = le16(h + 34);
    info.data_offset = 44;
    info.data_bytes = le32(h + 40);
    info.valid = info.bits == 16 && (info.channels == 1 || info.channels == 2);
    return info;
}

static size_t make_raw_chunk(FILE *file, const wav_info_t *info,
                             uint32_t *offset, uint8_t *out, size_t out_cap)
{
    uint32_t remaining = info->data_bytes - *offset;
    size_t to_read = remaining < out_cap ? remaining : out_cap;
    const uint32_t frame = info->channels * 2;
    to_read -= to_read % frame;
    if (to_read == 0 || fseek(file, info->data_offset + *offset, SEEK_SET) != 0) {
        return 0;
    }
    size_t n = fread(out, 1, to_read, file);
    n -= n % frame;
    *offset += (uint32_t)n;
    return n;
}

static size_t make_adpcm_chunk(FILE *file, const wav_info_t *info,
                               uint32_t *offset, uint8_t *out, size_t out_cap)
{
    uint8_t source[BLE_PCM_MAX_SAMPLES * 4];
    int16_t mono[BLE_PCM_MAX_SAMPLES];
    uint32_t remaining = info->data_bytes - *offset;
    size_t source_max = info->channels == 1 ? BLE_PCM_MAX_SAMPLES * 2
                                            : BLE_PCM_MAX_SAMPLES * 4;
    size_t to_read = remaining < source_max ? remaining : source_max;
    const uint32_t frame = info->channels * 2;
    to_read -= to_read % frame;
    if (to_read == 0 || fseek(file, info->data_offset + *offset, SEEK_SET) != 0) {
        return 0;
    }
    size_t n = fread(source, 1, to_read, file);
    n -= n % frame;
    if (n == 0) {
        return 0;
    }

    size_t samples = n / frame;
    for (size_t i = 0; i < samples; i++) {
        if (info->channels == 1) {
            mono[i] = (int16_t)le16(source + i * 2);
        } else {
            int16_t left = (int16_t)le16(source + i * 4);
            int16_t right = (int16_t)le16(source + i * 4 + 2);
            mono[i] = (int16_t)(((int32_t)left + (int32_t)right) / 2);
        }
    }

    size_t encoded = open_plaud_ble_adpcm_encode_block(mono, samples, out, out_cap);
    if (encoded > 0) {
        *offset += (uint32_t)n;
    }
    return encoded;
}

static int read_transfer_chunk(struct os_mbuf *om)
{
    if (s_cursor.path[0] == '\0' && !first_pending_clip(s_cursor.path, sizeof s_cursor.path)) {
        uint8_t hdr[BLE_CHUNK_HDR_LEN] = { 1 };
        return append_bytes(om, hdr, sizeof hdr);
    }

    FILE *file = fopen(s_cursor.path, "rb");
    if (!file) {
        uint8_t hdr[BLE_CHUNK_HDR_LEN] = { 2 };
        return append_bytes(om, hdr, sizeof hdr);
    }

    wav_info_t info = read_wav_info(file);
    if (!info.valid || s_cursor.offset >= info.data_bytes) {
        fclose(file);
        uint8_t hdr[BLE_CHUNK_HDR_LEN] = { 1 };
        op_put_le32(hdr + 4, s_cursor.offset);
        op_put_le32(hdr + 8, s_cursor.offset);
        op_put_le32(hdr + 12, info.data_bytes);
        return append_bytes(om, hdr, sizeof hdr);
    }

    uint8_t value[BLE_CHUNK_VALUE_MAX] = { 0 };
    uint8_t *payload = value + BLE_CHUNK_HDR_LEN;
    uint32_t start = s_cursor.offset;
    size_t payload_len = s_cursor.adpcm ?
        make_adpcm_chunk(file, &info, &s_cursor.offset, payload, BLE_CHUNK_PAYLOAD) :
        make_raw_chunk(file, &info, &s_cursor.offset, payload, BLE_CHUNK_PAYLOAD);
    fclose(file);

    value[0] = payload_len > 0 ? 0 : 2;
    value[1] = s_cursor.adpcm ? 1 : 0;
    op_put_le16(value + 2, (uint16_t)payload_len);
    op_put_le32(value + 4, start);
    op_put_le32(value + 8, s_cursor.offset);
    op_put_le32(value + 12, info.data_bytes);
    return append_bytes(om, value, BLE_CHUNK_HDR_LEN + payload_len);
}

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    chr_id_t id = (chr_id_t)(uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (id) {
        case CHR_STATUS: return read_status(ctxt->om);
        case CHR_CONFIG: return read_config(ctxt->om);
        case CHR_COMMAND: return read_command(ctxt->om);
        case CHR_TRANSFER_CONTROL: return read_transfer_control(ctxt->om);
        case CHR_TRANSFER_CHUNK: return read_transfer_chunk(ctxt->om);
        default: return BLE_ATT_ERR_UNLIKELY;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        switch (id) {
        case CHR_CONFIG: return write_config(ctxt->om);
        case CHR_COMMAND: return write_command(ctxt->om);
        case CHR_TRANSFER_CONTROL: return write_transfer_control(ctxt->om);
        default: return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_hs_adv_fields rsp_fields = { 0 };
    const char *name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response fields failed: %d", rc);
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(500),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(510),
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising as %s", CONFIG_OPEN_PLAUD_BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated to %d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "own address type failed: %d", rc);
        return;
    }
    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t open_plaud_ble_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    ESP_RETURN_ON_ERROR(err, TAG, "nimble_port_init");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT init failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(CONFIG_OPEN_PLAUD_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "device name failed: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    s_started = true;
    return ESP_OK;
}

void open_plaud_ble_set_status(const open_plaud_ble_status_t *status)
{
    if (!status) {
        return;
    }
    s_status = *status;
    if (s_started && s_status_handle != 0) {
        ble_gatts_chr_updated(s_status_handle);
    }
}

bool open_plaud_ble_take_command(open_plaud_ble_command_t *out)
{
    if (s_pending_command == OPEN_PLAUD_BLE_COMMAND_NONE) {
        if (out) {
            *out = OPEN_PLAUD_BLE_COMMAND_NONE;
        }
        return false;
    }
    if (out) {
        *out = s_pending_command;
    }
    s_pending_command = OPEN_PLAUD_BLE_COMMAND_NONE;
    return true;
}
#endif
