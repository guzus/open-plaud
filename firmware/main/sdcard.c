#include "sdcard.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;

esp_err_t sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        // Auto-format to FAT on first boot if the card has no FAT volume (e.g. a
        // blank card, or exFAT on a >32 GB card). Erases the card — fine for a
        // recordings card.
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (BSP_SD_WIDTH == 1) {
        host.flags = SDMMC_HOST_FLAG_1BIT;
    } else {
        host.flags = SDMMC_HOST_FLAG_4BIT;
    }
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    /* ESP32-S3 routes SDMMC through the GPIO matrix, so the pins are explicit. */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = BSP_SD_WIDTH;
    slot.clk   = BSP_SD_CLK_GPIO;
    slot.cmd   = BSP_SD_CMD_GPIO;
    slot.d0    = BSP_SD_D0_GPIO;
    if (BSP_SD_WIDTH == 4) {
        slot.d1 = BSP_SD_D1_GPIO;
        slot.d2 = BSP_SD_D2_GPIO;
        slot.d3 = BSP_SD_D3_GPIO;
    }
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (is a FAT-formatted card inserted?)",
                 esp_err_to_name(err));
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

esp_err_t sdcard_usage(uint64_t *out_used, uint64_t *out_total)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_bytes);
    if (err != ESP_OK) {
        return err;
    }
    if (out_total) {
        *out_total = total;
    }
    if (out_used) {
        *out_used = (total >= free_bytes) ? total - free_bytes : 0;
    }
    return ESP_OK;
}
