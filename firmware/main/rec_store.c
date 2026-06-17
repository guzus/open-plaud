/*
 * SD-side clip bookkeeping for the recorder loop. Numbered clips
 * (rec_NNNN.wav) in the card root are "recorded, not yet uploaded"; once a
 * clip reaches the server it is renamed into /sdcard/sent. Numbering scans
 * both directories, so numbers are never reused and clips are never
 * overwritten — across reboots, re-flashes, and failed uploads. The boot-time
 * retry pass drains the root oldest-first via net_upload_wav() and stops on
 * the first failure (Wi-Fi is likely down — don't burn the boot on timeouts).
 *
 * Everything here is plain POSIX VFS (opendir/readdir, rename, mkdir) on the
 * FATFS mount from sdcard.c; no heap allocation.
 */
#include "rec_store.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "net_upload.h"
#include "opus_ogg.h"
#include "sdcard.h"

static const char *TAG = "rec_store";

#define SENT_DIR        SDCARD_MOUNT_POINT "/sent"
#define REC_NUM_MAX     9999u  /* rec_0001 .. rec_9999 — 8.3-safe names */
#define RETRY_BATCH_MAX 64     /* pending clips retried per boot */
#define WAV_HEADER_BYTES 44

typedef struct {
    rec_store_progress_fn progress;
    void *ctx;
    int uploaded;
    int total;
    int last_pct;
} upload_progress_ctx_t;

static void upload_progress(const char *path, int sent_bytes, int total_bytes,
                            net_upload_phase_t phase, void *ctx)
{
    upload_progress_ctx_t *p = (upload_progress_ctx_t *)ctx;
    if (!p || !p->progress) {
        return;
    }
    switch (phase) {
    case NET_UPLOAD_PHASE_WIFI_CONNECTING:
        p->progress(p->uploaded, p->total, -1, path,
                    REC_STORE_PROGRESS_WIFI_CONNECTING, p->ctx);
        return;
    case NET_UPLOAD_PHASE_WIFI_CONNECTED:
        p->progress(p->uploaded, p->total, -1, path,
                    REC_STORE_PROGRESS_WIFI_CONNECTED, p->ctx);
        return;
    case NET_UPLOAD_PHASE_WIFI_FAILED:
        p->progress(p->uploaded, p->total, -1, path,
                    REC_STORE_PROGRESS_WIFI_FAILED, p->ctx);
        return;
    case NET_UPLOAD_PHASE_UPLOADING:
        break;
    }
    if (total_bytes <= 0) {
        return;
    }
    int pct = sent_bytes * 100 / total_bytes;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == p->last_pct || (pct < 100 && pct - p->last_pct < 5)) {
        return;
    }
    p->last_pct = pct;
    p->progress(p->uploaded, p->total, pct, path,
                REC_STORE_PROGRESS_UPLOADING, p->ctx);
}

/* Match "rec_NNNN.wav" case-insensitively: FATFS is a case-insensitive
 * filesystem, and with long-filename support off it stores 8.3 names
 * uppercase, so readdir may return "REC_0001.WAV". Digits are parsed by hand
 * (not sscanf) so "rec_-12a.wav" and friends are rejected. */
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

/* Highest clip number in dir; 0 if none (or if dir cannot be opened). */
static unsigned scan_max_num(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    unsigned max = 0, num;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (parse_rec_name(ent->d_name, &num) && num > max) max = num;
    }
    closedir(d);
    return max;
}

int rec_store_pending_count(void)
{
    DIR *d = opendir(SDCARD_MOUNT_POINT);
    if (!d) {
        return 0;
    }
    int n = 0;
    unsigned num;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (parse_rec_name(ent->d_name, &num)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

esp_err_t rec_store_init(void)
{
    if (mkdir(SENT_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d", SENT_DIR, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rec_store_next_path(char *out, size_t out_len)
{
    unsigned root_max = scan_max_num(SDCARD_MOUNT_POINT);
    unsigned sent_max = scan_max_num(SENT_DIR);
    unsigned next = (root_max > sent_max ? root_max : sent_max) + 1;
    if (next > REC_NUM_MAX) {
        ESP_LOGE(TAG, "clip numbers exhausted (rec_%04u.wav) — archive this SD "
                      "card or clear rec_*.wav from / and /sent, then reboot",
                 REC_NUM_MAX);
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(out, out_len, SDCARD_MOUNT_POINT "/rec_%04u.wav", next);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t rec_store_mark_sent(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char dest[40];
    int n = snprintf(dest, sizeof dest, SENT_DIR "/%s", base);
    if (n < 0 || (size_t)n >= sizeof dest) return ESP_ERR_INVALID_SIZE;
    if (rename(path, dest) != 0) {  /* same FATFS volume: a metadata-only move */
        ESP_LOGE(TAG, "rename(%s -> %s) failed: errno=%d", path, dest, errno);
        return ESP_FAIL;
    }

#if CONFIG_OPEN_PLAUD_OPUS_TRANSFERS
    char sidecar[48];
    char sidecar_dest[48];
    if (opus_ogg_path_for_wav(path, sidecar, sizeof(sidecar)) == ESP_OK &&
        opus_ogg_path_for_wav(dest, sidecar_dest, sizeof(sidecar_dest)) == ESP_OK) {
        struct stat st;
        if (stat(sidecar, &st) == 0) {
            remove(sidecar_dest);
            if (rename(sidecar, sidecar_dest) != 0) {
                ESP_LOGW(TAG, "rename(%s -> %s) failed: errno=%d",
                         sidecar, sidecar_dest, errno);
            }
        }
    }
#endif
    return ESP_OK;
}

int rec_store_retry_pending(bool *out_upload_failed,
                            rec_store_progress_fn progress, void *ctx)
{
    if (out_upload_failed) *out_upload_failed = false;

    /* Collect pending clip numbers — fixed array, no heap. If more than
     * RETRY_BATCH_MAX are pending, keep the oldest by evicting the newest. */
    unsigned pending[RETRY_BATCH_MAX];
    size_t count = 0;
    unsigned total = 0, num;

    DIR *d = opendir(SDCARD_MOUNT_POINT);
    if (!d) {
        ESP_LOGE(TAG, "opendir(%s) failed: errno=%d", SDCARD_MOUNT_POINT, errno);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!parse_rec_name(ent->d_name, &num)) continue;
        total++;
        if (count < RETRY_BATCH_MAX) {
            pending[count++] = num;
        } else {
            size_t newest = 0;
            for (size_t i = 1; i < count; i++) {
                if (pending[i] > pending[newest]) newest = i;
            }
            if (num < pending[newest]) pending[newest] = num;
        }
    }
    closedir(d);

    if (count == 0) return 0;
    if (total > RETRY_BATCH_MAX) {
        ESP_LOGW(TAG, "%u clips pending; retrying the oldest %d this boot",
                 total, RETRY_BATCH_MAX);
    }

    /* Oldest number first (insertion sort; count <= RETRY_BATCH_MAX). */
    for (size_t i = 1; i < count; i++) {
        unsigned v = pending[i];
        size_t j = i;
        while (j > 0 && pending[j - 1] > v) { pending[j] = pending[j - 1]; j--; }
        pending[j] = v;
    }

    int uploaded = 0;
    for (size_t i = 0; i < count; i++) {
        char path[32];
        snprintf(path, sizeof path, SDCARD_MOUNT_POINT "/rec_%04u.wav", pending[i]);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size <= WAV_HEADER_BYTES) {
            ESP_LOGW(TAG, "discarding empty clip %s (%ld bytes)", path, (long)st.st_size);
            if (remove(path) != 0) {
                ESP_LOGE(TAG, "remove(%s) failed: errno=%d", path, errno);
                if (out_upload_failed) *out_upload_failed = true;
                break;
            }
            uploaded++;
            if (progress) {
                progress(uploaded, (int)count, -1, NULL,
                         REC_STORE_PROGRESS_CLIP_DONE, ctx);
            }
            continue;
        }

        upload_progress_ctx_t progress_ctx = {
            .progress = progress,
            .ctx = ctx,
            .uploaded = uploaded,
            .total = (int)count,
            .last_pct = -5,
        };
        if (net_upload_wav(path, upload_progress, &progress_ctx) != ESP_OK) {
            ESP_LOGW(TAG, "retry stopped at %s — %u clip(s) stay queued",
                     path, (unsigned)(count - i));
            if (out_upload_failed) *out_upload_failed = true;
            break;
        }
        if (rec_store_mark_sent(path) != ESP_OK) {
            ESP_LOGW(TAG, "%s uploaded but not moved to %s — it may upload again next boot",
                     path, SENT_DIR);
        }
        uploaded++;
        if (progress) {
            progress(uploaded, (int)count, -1, NULL,
                     REC_STORE_PROGRESS_CLIP_DONE, ctx);
        }
    }
    return uploaded;
}
