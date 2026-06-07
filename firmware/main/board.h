#pragma once
/*
 * Waveshare ESP32-S3-LCD-1.54 pin map.
 *
 * Current v0 hardware target: ESP32-S3-LCD-1.54 with the bundled battery SKU.
 * Confidence: Waveshare ESP-IDF BSP + schematic for ESP32-S3-Touch-LCD-1.54.
 * The non-touch and touch variants share the same core board features; touch
 * adds CST816 on the same I2C bus.
 */

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
#define BSP_SD_WIDTH          1

/* ---- LCD (ST7789, SPI; unused by the recorder bring-up path today) ------ */
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
#define BSP_PA_CTRL_GPIO      7    /* speaker amp enable (playback)           */
#define BSP_BAT_ADC_GPIO      1
#define BSP_BAT_EN_GPIO       2
#define BSP_CHG_STAT_GPIO     3
