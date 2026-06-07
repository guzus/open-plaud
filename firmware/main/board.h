#pragma once
/*
 * Waveshare ESP32-S3-AUDIO-Board pin map.
 *
 * Confidence: schematic-confirmed against ESP32-S3-AUDIO-Board_1.1.pdf and
 * cross-checked against an ESPHome config for this exact board. Sources are
 * listed in firmware/README.md. Pins marked UNKNOWN are not wired to a labelled
 * GPIO in the published schematic — measure/confirm before relying on them.
 */

/* ---- I2S0 (single shared full-duplex bus; the recorder uses RX only) ---- */
#define BSP_I2S_MCLK_GPIO    12
#define BSP_I2S_BCLK_GPIO    13
#define BSP_I2S_WS_GPIO      14
#define BSP_I2S_ASDOUT_GPIO  15   /* ES7210 ADC  -> ESP RX  (microphones)     */
#define BSP_I2S_DSDIN_GPIO   16   /* ESP TX -> ES8311 DAC (playback; unused)  */

/* ---- I2C0 (every on-board chip shares this bus) ------------------------- */
#define BSP_I2C_SDA_GPIO     11
#define BSP_I2C_SCL_GPIO     10
/* 8-bit (left-shifted) addresses — what esp_codec_dev expects: */
#define BSP_ES7210_ADDR      0x80 /* 7-bit 0x40, mic ADC                      */
#define BSP_ES8311_ADDR      0x30 /* 7-bit 0x18, DAC                          */
#define BSP_PCF85063_ADDR_7B 0x51 /* RTC                                      */
#define BSP_TCA9555_ADDR_7B  0x20 /* IO expander: buttons, PA, USB/UART mux   */

/* ---- microSD (SDMMC, 1-bit; D1/D2 not GPIO-wired, D3 sits on the expander) */
#define BSP_SD_CLK_GPIO      40
#define BSP_SD_CMD_GPIO      42
#define BSP_SD_D0_GPIO       41
#define BSP_SD_WIDTH         1

/* ---- WS2812B RGB ring --------------------------------------------------- */
#define BSP_WS2812_GPIO      38
#define BSP_WS2812_COUNT     7

/* ---- TCA9555 expander bits (NOT direct GPIOs — drive over I2C) ----------
 * Buttons + speaker-amp enable live on the expander, so a future hold-to-record
 * button reads these via the TCA9555, not gpio_get_level().
 */
#define BSP_EXIO_SD_D3       3
#define BSP_EXIO_RTC_INT     4
#define BSP_EXIO_USB_UART_MUX 7   /* default HIGH = native USB on GPIO19/20   */
#define BSP_EXIO_PA_CTRL     8    /* speaker amp enable (playback)            */
#define BSP_EXIO_KEY1        9
#define BSP_EXIO_KEY2        10
#define BSP_EXIO_KEY3        11
