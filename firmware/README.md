# Firmware — ESP-IDF (Waveshare ESP32-S3-AUDIO-Board)

The recorder firmware. ESP-IDF (C), no Arduino. Current milestone: **boot → mount
microSD → init the ES7210 dual-mic ADC → record a fixed WAV clip to the card.**
It then joins Wi-Fi and uploads that WAV to the server as multipart/form-data.

> **Status: scaffold, not yet flashed on hardware.** Pins are schematic-confirmed,
> but `audio.c` mirrors Espressif's `i2s_es7210_tdm` example from source — verify the
> `esp_codec_dev` struct fields against your installed version before trusting a build.
> This is a real starting point to iterate on with a board in hand, not a finished image.

## Prerequisites

[ESP-IDF **v5.4.x**](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/)
(Waveshare targets v5.4.1):

```bash
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh        # run in each new shell (adds idf.py to PATH)
```

## Build · flash · monitor

This board uses the ESP32-S3's **native USB-Serial/JTAG** (no UART bridge chip), so
just plug the USB-C port into your Mac — it appears as `/dev/cu.usbmodem*`.

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig                         # open-plaud: SSID/password/server URL
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor    # Ctrl-] to exit the monitor
```

`idf.py` pulls the `esp_codec_dev` managed component on first build. If the port
doesn't appear, hold **BOOT**, tap **RESET**, release BOOT to force download mode.

Insert a FAT32 microSD. On boot you should see the SD info, `audio ready: 16000 Hz…`,
then `wrote N bytes to /sdcard/rec_0001.wav`, followed by a multipart upload to
`POST {server}/v1/recordings`. If Wi-Fi or HTTP fails, the WAV remains on the SD
card for manual recovery or a future retry queue.

## Layout

| File | Role |
|---|---|
| `main/board.h` | **pin map** (schematic-confirmed) — I2S/I2C/SD + TCA9555 expander bits |
| `main/audio.c` | ES7210 dual-mic capture (I2C via `esp_codec_dev`, I2S-TDM RX) |
| `main/sdcard.c` | mount microSD (FATFS, **1-bit** SDMMC: CLK=40, CMD=42, D0=41) |
| `main/wav.c` | streaming 16-bit PCM WAV writer |
| `main/net_upload.c` | Wi-Fi STA + multipart upload to the server |
| `main/app_main.c` | orchestration: mount → record → (upload) |
| `sdkconfig.defaults` | 16 MB flash, octal PSRAM, USB-JTAG console, custom partitions |

## Device → server contract (target)

The device is "dumb on purpose" (see `docs/ARCHITECTURE.md`): record locally, then
upload raw audio. The server does transcription/summarization.

```
POST {OP_SERVER}/v1/recordings
Content-Type: multipart/form-data
  file=@rec_0001.wav            # 16 kHz, 16-bit, 2-channel PCM WAV
→ 200 { "id": "...", "status": "transcribing", "filename": "rec_0001.wav", "size_bytes": ... }
```

Firmware settings live under `idf.py menuconfig` → **open-plaud**:

- `OPEN_PLAUD_WIFI_SSID`
- `OPEN_PLAUD_WIFI_PASSWORD`
- `OPEN_PLAUD_SERVER_URL` (base URL, for example `http://192.168.1.50:8000`)
- `OPEN_PLAUD_WIFI_MAX_RETRIES`
- `OPEN_PLAUD_UPLOAD_TIMEOUT_MS`

## TODO (ROADMAP v1)

- [ ] Flash on hardware and tune the first real Wi-Fi/audio bring-up issues.
- [ ] Hold-to-record button — **on the TCA9555 expander** (`BSP_EXIO_KEY1`), read over
      I2C, not a GPIO. Speaker amp enable (`BSP_EXIO_PA_CTRL`) is there too.
- [ ] Retry queue, rotate filenames / tag recordings; RGB ring (GPIO38, 7×WS2812) as status.
- [ ] Confirm SD at higher clock; D1/D2 aren't GPIO-wired so stay 1-bit.

## Sources

- Pin map: Waveshare schematic `ESP32-S3-AUDIO-Board_1.1.pdf`
  ([wiki](https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board)), cross-checked vs an
  [ESPHome config](https://github.com/ichbaum/esphome-esp32_s3_audio_board) for this board.
- Capture recipe: ESP-IDF `examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm`.
