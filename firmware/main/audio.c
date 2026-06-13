/*
 * Dual-mic capture. The Waveshare v0 target uses ES7210 over I2S-TDM. The
 * custom Rev A compile profile uses ESP32-S3 PDM RX directly from MEMS mics.
 *
 * NOTE: compiled with ESP-IDF v5.4.1 + the esp_codec_dev managed component, but
 * not yet run on hardware. Both audio paths still need board validation.
 */
#include "audio.h"
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#if BSP_AUDIO_BACKEND_ES7210
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#endif

#if BSP_AUDIO_BACKEND_PDM
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "soc/soc_caps.h"
#endif

static const char *TAG = "audio";

static i2s_chan_handle_t      s_rx;

#if BSP_AUDIO_BACKEND_ES7210
static i2c_master_bus_handle_t s_i2c;
static esp_codec_dev_handle_t s_codec;

static esp_err_t tdm_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel");

    i2s_tdm_config_t tdm = {
        .clk_cfg = {
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        /* 2 mics -> 2 TDM slots (ES7210 uses Philips framing). */
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK_GPIO,
            .bclk = BSP_I2S_BCLK_GPIO,
            .ws   = BSP_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,          /* recorder: no TX */
            .din  = BSP_I2S_ASDOUT_GPIO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &tdm), TAG, "init_tdm");
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port   = 0,
        .sda_io_num = BSP_I2C_SDA_GPIO,
        .scl_io_num = BSP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c_bus");

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = 0, .addr = BSP_ES7210_ADDR, .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(ctrl, ESP_FAIL, TAG, "i2c_ctrl");

    audio_codec_i2s_cfg_t i2s_data_cfg = { .port = 0, .rx_handle = s_rx, .tx_handle = NULL };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(data, ESP_FAIL, TAG, "i2s_data");

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if      = ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .master_mode  = false,
        .mclk_src     = ES7210_MCLK_FROM_PAD,
        .mclk_div     = I2S_MCLK_MULTIPLE_256,
    };
    const audio_codec_if_t *es = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(es, ESP_FAIL, TAG, "es7210_new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es, .data_if = data,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec_dev_new");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .channel_mask    = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec, &fs), TAG, "codec_open");
    esp_codec_dev_set_in_gain(s_codec, 30.0);   /* dB mic gain */
    return ESP_OK;
}
#endif

#if BSP_AUDIO_BACKEND_PDM
static esp_err_t mic_power_init(void)
{
    if (BSP_MIC_EN_GPIO < 0) {
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_MIC_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "mic_en gpio");
    gpio_set_level(BSP_MIC_EN_GPIO, 1);
    return ESP_OK;
}

static esp_err_t pdm_init(void)
{
    ESP_RETURN_ON_ERROR(mic_power_init(), TAG, "mic_power");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel");

    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = BSP_PDM_CLK_GPIO,
#if SOC_I2S_PDM_MAX_RX_LINES == 4
            .dins = {
                BSP_PDM_DATA0_GPIO,
                GPIO_NUM_NC,
                GPIO_NUM_NC,
                GPIO_NUM_NC,
            },
#else
            .din = BSP_PDM_DATA0_GPIO,
#endif
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    pdm.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    pdm.slot_cfg.slot_mask = I2S_PDM_SLOT_BOTH;

    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx, &pdm), TAG, "init_pdm");
    return ESP_OK;
}
#endif

esp_err_t audio_init(void)
{
#if BSP_AUDIO_BACKEND_ES7210
    ESP_RETURN_ON_ERROR(tdm_init(), TAG, "tdm");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec");
#elif BSP_AUDIO_BACKEND_PDM
    ESP_RETURN_ON_ERROR(pdm_init(), TAG, "pdm");
#else
#error "No audio backend selected"
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
    ESP_LOGI(
        TAG,
        "audio ready (%s): %d Hz, %d-bit, %d ch",
        BSP_BOARD_NAME,
        AUDIO_SAMPLE_RATE,
        AUDIO_BITS,
        AUDIO_CHANNELS
    );
    return ESP_OK;
}

esp_err_t audio_read(void *buf, size_t bytes, size_t *out_read)
{
    /* Finite timeout, not portMAX_DELAY: a wedged DMA (codec crash, clock loss)
     * must surface so the caller's bad-read bound can abort the clip. A timeout
     * can still return partial data in *out_read; callers should use it.
     */
    return i2s_channel_read(
        s_rx, buf, bytes, out_read, pdMS_TO_TICKS(CONFIG_OPEN_PLAUD_AUDIO_READ_TIMEOUT_MS)
    );
}

void audio_deinit(void)
{
#if BSP_AUDIO_BACKEND_ES7210
    if (s_codec) { esp_codec_dev_close(s_codec); esp_codec_dev_delete(s_codec); s_codec = NULL; }
#endif
    if (s_rx)    { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); s_rx = NULL; }
#if BSP_AUDIO_BACKEND_ES7210
    if (s_i2c)   { i2c_del_master_bus(s_i2c); s_i2c = NULL; }
#endif
}
