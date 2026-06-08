# Hardware decision — v0 recorder

## Decision

Use **Waveshare ESP32-S3-LCD-1.54 with battery** as the v0 open-plaud hardware
target.

## Why this board

First-principles constraints:

- Recorder audio matters more than display type.
- The board must record locally, then upload over Wi-Fi.
- Battery/charging should be integrated for the first portable prototype.
- The BOM should stay under $30 for the core board when possible.
- Thinness matters, but a reliable recorder prototype matters more than a
  prematurely custom PCB.

The LCD-1.54 board satisfies the core constraints:

| Constraint | LCD-1.54 answer |
|---|---|
| Core price | $15.99 direct for SKU 33867, shipping separate |
| Audio | dual microphone capture through ES7210 ADC |
| Storage | onboard TF/microSD slot |
| Upload | ESP32-S3 Wi-Fi |
| Battery | onboard 3.7V lithium battery charge/discharge connector; battery SKU available |
| UI | built-in 1.54" LCD for status, battery, recording state |

## Compared options

| Option | Keep? | Reason |
|---|---|---|
| ESP32-S3-LCD-1.54 + battery | **Primary v0** | Best balance of mic, battery, storage, UI, and price |
| ESP32-S3-AUDIO-Board + battery | Reference only | Strong audio board but bulkier and less product-like |
| ESP32-S3-ePaper-1.54 + battery | Not v0 | Better low-power display, weaker single-mic recorder path |
| XIAO ESP32-S3 Sense + LiPo | Not v0 | Smaller footprint, but single mic and stacked module hurt recorder quality |

## Firmware impact

The previous firmware scaffold targeted the ESP32-S3-AUDIO-Board. The LCD-1.54
uses the same class of audio parts, but a different pin map. `firmware/main/board.h`
now follows Waveshare's ESP-IDF BSP for LCD-1.54:

| Signal | GPIO |
|---|---:|
| I2S MCLK/BCLK/LRCK/ADC OUT/DAC IN | 8 / 9 / 10 / 11 / 12 |
| I2C SDA/SCL | 42 / 41 |
| SDMMC CLK/CMD/D0 | 16 / 15 / 17 |
| LCD MOSI/SCLK/CS/DC/RST/BL | 39 / 38 / 21 / 45 / 40 / 46 |
| Buttons BOOT/PLUS/PWR | 0 / 4 / 5 |

Next firmware milestone: flash on the LCD-1.54 board, validate ES7210 init and
SD write path, then add PLUS-button hold-to-record and LCD status.

## Sources

- Waveshare product SKU: https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33867
- Waveshare docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54
- Waveshare example BSP: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54
- Schematic: `ESP32-S3-LCD-1.54-Schematic.pdf` from Waveshare resources
