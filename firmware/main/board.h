#pragma once
#include "sdkconfig.h"
#if CONFIG_OPEN_PLAUD_BOARD_CUSTOM_REV_A

/*
 * Custom PCB Rev A proposed pin map.
 *
 * Confidence: docs/CUSTOM_PCB_REV_A.md architecture contract. This validates
 * ESP-IDF API compatibility for the proposed pins; it is not an ERC/DRC result.
 */
#define BSP_BOARD_NAME          "custom-rev-a"
#define BSP_AUDIO_BACKEND_PDM   1
#define BSP_AUDIO_BACKEND_ES7210 0
#define BSP_HAS_LCD             0
#define BSP_HAS_RGB_LED         1

/* ---- I2S0 PDM microphones --------------------------------------------- */
#define BSP_PDM_CLK_GPIO       8
#define BSP_PDM_DATA0_GPIO     9
#define BSP_PDM_DATA1_GPIO    10    /* optional future pair, not used by v1 capture */
#define BSP_MIC_EN_GPIO       11

/* ---- microSD / TF card (SDMMC; 1-bit default, 4-bit after validation) --- */
#define BSP_SD_CLK_GPIO       14
#define BSP_SD_CMD_GPIO       15
#define BSP_SD_D0_GPIO        16
#define BSP_SD_D1_GPIO        17
#define BSP_SD_D2_GPIO        18
#define BSP_SD_D3_GPIO        21
#define BSP_SD_DET_GPIO        7
#define BSP_SD_WIDTH           CONFIG_OPEN_PLAUD_SDMMC_WIDTH

/* ---- Fuel gauge / power status ----------------------------------------- */
#define BSP_I2C_SDA_GPIO      42
#define BSP_I2C_SCL_GPIO      41
#define BSP_CHG_STAT_GPIO      5
#define BSP_PGOOD_GPIO         6

/* ---- Buttons and RGB status LED ---------------------------------------- */
#define BSP_BOOT_BUTTON_GPIO   0
#define BSP_PLUS_BUTTON_GPIO   4
#define BSP_BTN_ACTIVE_LEVEL   0
/* This board exposes no third (power) button, so Left aliases the record button
 * (Right) to keep button.c compiling. The UX degrades safely: record_clip gates
 * Cancel on stop_armed and checks Stop first, so Right still records/stops and
 * the aliased Left simply never cancels. (No LCD here either — BSP_HAS_LCD 0.) */
#define BSP_BTN_RIGHT_GPIO    BSP_PLUS_BUTTON_GPIO
#define BSP_BTN_MIDDLE_GPIO   BSP_BOOT_BUTTON_GPIO
#define BSP_BTN_LEFT_GPIO     BSP_PLUS_BUTTON_GPIO
#define BSP_RGB_LED_R_GPIO    35
#define BSP_RGB_LED_G_GPIO    36
#define BSP_RGB_LED_B_GPIO    37
#define BSP_RGB_LED_ACTIVE_LEVEL 1

#else

/*
 * Waveshare ESP32-S3-LCD-1.54 pin map.
 *
 * Current v0 hardware target: ESP32-S3-LCD-1.54 with the bundled battery SKU.
 * Confidence: Waveshare ESP-IDF BSP + schematic for ESP32-S3-Touch-LCD-1.54.
 * The non-touch and touch variants share the same core board features; touch
 * adds CST816 on the same I2C bus.
 */
#define BSP_BOARD_NAME          "waveshare-lcd154"
#define BSP_AUDIO_BACKEND_PDM   0
#define BSP_AUDIO_BACKEND_ES7210 1
#define BSP_HAS_LCD             1
#define BSP_HAS_RGB_LED         0

/* ---- I2S0 (single shared full-duplex bus; the recorder uses RX only) ---- */
#define BSP_I2S_MCLK_GPIO     8
#define BSP_I2S_BCLK_GPIO     9
#define BSP_I2S_WS_GPIO      10
#define BSP_I2S_ASDOUT_GPIO  11   /* ES7210 ADC  -> ESP RX  (microphones)     */
#define BSP_I2S_DSDIN_GPIO   12   /* ESP TX -> ES8311 DAC (playback; unused)  */

/* ---- I2C0 (codec, IMU, and touch controller on touch SKU) --------------- */
#define BSP_I2C_SDA_GPIO     42
#define BSP_I2C_SCL_GPIO     41
/* 8-bit (left-shifted) addresses — what esp_codec_dev expects: */
#define BSP_ES7210_ADDR      0x80 /* 7-bit 0x40, mic ADC                      */
#define BSP_ES8311_ADDR      0x30 /* 7-bit 0x18, DAC                          */
#define BSP_CST816_ADDR_7B   0x15 /* touch controller, touch SKU only         */

/* ---- microSD / TF card (SDMMC; recorder uses 1-bit mode for conservative bring-up) */
#define BSP_SD_CLK_GPIO      16
#define BSP_SD_CMD_GPIO      15
#define BSP_SD_D0_GPIO       17
#define BSP_SD_D1_GPIO       18
#define BSP_SD_D2_GPIO       13
#define BSP_SD_D3_GPIO       14
#define BSP_SD_WIDTH          CONFIG_OPEN_PLAUD_SDMMC_WIDTH

/* ---- LCD (ST7789, SPI; unused by the recorder bring-up path today) ------ */
#define BSP_LCD_H_RES       240
#define BSP_LCD_V_RES       240
#define BSP_LCD_MOSI_GPIO    39
#define BSP_LCD_SCLK_GPIO    38
#define BSP_LCD_CS_GPIO      21
#define BSP_LCD_DC_GPIO      45
#define BSP_LCD_RST_GPIO     40
#define BSP_LCD_BL_GPIO      46

/* ---- Buttons / power / battery ----------------------------------------- */
#define BSP_BOOT_BUTTON_GPIO  0
#define BSP_PLUS_BUTTON_GPIO  4
#define BSP_PWR_BUTTON_GPIO   5
/* UNVERIFIED ASSUMPTION — confirm on first hardware bring-up. The PLUS button
 * is assumed wired button-to-GND, idling high via the ESP32-S3 internal
 * pull-up, so pressed == level 0. The schematic has not been cross-checked for
 * this net. If a recording starts by itself at boot, or a held button never
 * starts one, flip this to 1 — see "Validate on first hardware bring-up" in
 * firmware/README.md. */
#define BSP_BTN_ACTIVE_LEVEL  0

/* Top-edge physical button positions for the 3-button UX. button.c maps
 * Right->record/stop, Middle->sync, Left->power/screen, and status.c draws each
 * button's label in that left/middle/right column. RIGHT is anchored: GPIO4 has
 * been the record button since v0. LEFT/MIDDLE are GPIO5/GPIO0, but WHICH IS
 * PHYSICALLY WHICH IS UNVERIFIED — same confidence as BSP_BTN_ACTIVE_LEVEL.
 * Boot logs the GPIO behind each label; if the on-screen labels point at the
 * wrong buttons on real hardware, swap the LEFT and MIDDLE lines below. */
#define BSP_BTN_RIGHT_GPIO    BSP_PLUS_BUTTON_GPIO   /* record/stop (anchored)      */
#define BSP_BTN_MIDDLE_GPIO   BSP_BOOT_BUTTON_GPIO   /* sync         (assumed middle)*/
#define BSP_BTN_LEFT_GPIO     BSP_PWR_BUTTON_GPIO    /* power/screen (assumed left)  */

#define BSP_PA_CTRL_GPIO      7    /* speaker amp enable (playback)           */
#define BSP_BAT_ADC_GPIO      1
#define BSP_BAT_EN_GPIO       2
#define BSP_CHG_STAT_GPIO     3

#endif
