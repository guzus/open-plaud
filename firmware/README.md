# Firmware — ESP-IDF recorder

The recorder firmware. ESP-IDF (C), no Arduino. Current milestone: **boot →
retry any uploads a previous run left pending → hold the PLUS button to record
a numbered WAV (`rec_NNNN.wav`) to microSD → upload on release** as
multipart/form-data. Uploaded clips move to `/sdcard/sent/`; failed uploads
stay in the card root and are retried on the next boot.

> **Status: both board profiles build with ESP-IDF v5.4.1, not yet flashed on
> hardware.** The Waveshare ES7210/TDM path and custom Rev A PDM path still need
> first-board validation before treating either image as finished.

## Prerequisites

[ESP-IDF **v5.4.x+**](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/).
Waveshare's LCD-1.54 examples currently target ESP-IDF 5.5.1, while this repo
currently builds on the v5.4.x line until hardware says otherwise.

```bash
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
. ~/esp/esp-idf/export.sh        # run in each new shell (adds idf.py to PATH)
```

## Build · flash · monitor

The selected v0 board uses the ESP32-S3's **native USB-Serial/JTAG** (no UART
bridge chip), so just plug the USB-C port into your Mac — it appears as
`/dev/cu.usbmodem*`.

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig                         # open-plaud: SSID/password/server URL
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor    # Ctrl-] to exit the monitor
```

To compile the custom PCB Rev A profile against the proposed PDM mic pin map:

```bash
idf.py -B build-rev-a -DSDKCONFIG=build-rev-a/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rev_a.defaults" build
```

`idf.py` pulls managed components on first build. If the port doesn't appear,
hold **BOOT**, tap **RESET**, release BOOT to force download mode.

Insert a FAT32 microSD. On boot you should see the SD info, `audio ready: 16000 Hz…`,
a retry pass over clips left pending by earlier runs (oldest first, up to 64 per
boot), then `ready — hold PLUS (GPIO4) to record`. Hold **PLUS** to record
`rec_0001.wav`, `rec_0002.wav`, … (numbers are never reused — the root *and*
`/sdcard/sent/` are scanned); release to stop and upload to
`POST {server}/v1/recordings`. On success the clip moves to `/sdcard/sent/`. On
the first upload failure the firmware stops trying for the rest of this boot
(Wi-Fi is likely down) and keeps recording to SD; the next boot's retry pass
clears the backlog. A held button auto-stops after
`OPEN_PLAUD_MAX_REC_SECONDS` (default 1800 s) and a new press starts a new clip.

## Layout

| File | Role |
|---|---|
| `main/board.h` | **pin maps** for Waveshare LCD-1.54 and custom PCB Rev A |
| `main/audio.c` | ES7210/TDM capture for Waveshare; direct PDM RX for Rev A |
| `main/button.c` | PLUS button (GPIO4) — polled input + pull-up, raw level only; `app_main` debounces |
| `main/sdcard.c` | mount microSD (FATFS over configurable 1-bit/4-bit SDMMC) |
| `main/wav.c` | streaming 16-bit PCM WAV writer |
| `main/rec_store.c` | clip numbering (`rec_NNNN.wav`, never reused), `/sdcard/sent/` move, boot-time retry queue |
| `main/net_upload.c` | Wi-Fi STA + multipart upload to the server |
| `main/status.c` | LCD or RGB LED status states (boot, ready, recording, syncing, done, error) |
| `main/app_main.c` | orchestration: mount → retry pending → hold-to-record loop → upload |
| `sdkconfig.defaults` | 16 MB flash, octal PSRAM, USB-JTAG console, custom partitions |
| `sdkconfig.rev_a.defaults` | custom PCB Rev A compile profile |

## Device → server contract (target)

The device is "dumb on purpose" (see `docs/ARCHITECTURE.md`): record locally, then
upload raw audio. The server does transcription/summarization.

```
POST {OP_SERVER}/v1/recordings
Content-Type: multipart/form-data
  file=@rec_0001.wav            # 16 kHz, 16-bit, 2-channel PCM WAV
→ 200 { "id": "...", "status": "received", "filename": "rec_0001.wav", "size_bytes": ... }
       # the server transcribes/summarizes in the background; poll GET for the result
```

Firmware settings live under `idf.py menuconfig` → **open-plaud**:

- `OPEN_PLAUD_BOARD_WAVESHARE_LCD154` / `OPEN_PLAUD_BOARD_CUSTOM_REV_A`
- `OPEN_PLAUD_WIFI_SSID`
- `OPEN_PLAUD_WIFI_PASSWORD`
- `OPEN_PLAUD_SERVER_URL` (base URL, for example `http://192.168.1.50:8000`)
- `OPEN_PLAUD_WIFI_MAX_RETRIES`
- `OPEN_PLAUD_UPLOAD_TIMEOUT_MS`
- `OPEN_PLAUD_MAX_REC_SECONDS` (auto-stop for a held button, default 1800)
- `OPEN_PLAUD_AUDIO_READ_TIMEOUT_MS`
- `OPEN_PLAUD_AUDIO_READ_MAX_ERRORS`
- `OPEN_PLAUD_SDMMC_WIDTH` (1-bit default; 4-bit only after hardware validation)

## Validate on first hardware bring-up

A source review plus local ESP-IDF build found the upload seam correct —
multipart field `file`, `.wav`, path `/v1/recordings` all match the server. Open
items to confirm with a board in hand:

- **Audio init (highest uncertainty).** `audio.c` drives both `esp_codec_dev_open`
  *and* a raw `i2s_channel_enable` over a hand-rolled TDM config. Depending on the
  installed `esp_codec_dev` version these can double-configure/enable the RX path —
  watch for an error at `i2s_channel_enable`/`esp_codec_dev_open` and drop one side
  if so.
- **PLUS button polarity (`BSP_BTN_ACTIVE_LEVEL` in `board.h`).** UNVERIFIED
  assumption: button shorts GPIO4 to GND and idles high on the internal pull-up,
  so pressed == 0. If a recording starts by itself at boot, or a held button
  never starts one, flip it to 1.
- **FATFS rename-into-subdir.** `rec_store_mark_sent()` relies on
  `rename("/sdcard/rec_NNNN.wav" → "/sdcard/sent/…")` across directories on the
  same volume (FatFs `f_rename` supports this). Confirm clips actually land in
  `/sdcard/sent/` after a successful upload.
- **Debounce timing.** Start/stop needs 2 consecutive agreeing polls: 20 ms
  apart when idle, one poll per ~64 ms audio chunk while recording. Verify
  against the real button's bounce; tune `BTN_POLL_MS`/`BTN_DEBOUNCE_POLLS`.
- **`audio_read` stall bound.** Reads time out at 500 ms (not `portMAX_DELAY` —
  a wedged DMA must surface, or the task blocks forever, invisible to the task
  watchdog); a clip aborts after 20 consecutive empty reads (~10 s worst case).
  Both numbers are untested — tune once you see real I2S failure behavior.
- **Transcription cost:** capture is 2-channel; consider down-mixing the two mics
  to mono before upload (halves file size and cloud-Whisper cost) unless
  diarization needs both. Not a correctness issue: server-side `whisper_cpp` was
  verified to transcribe device-shaped stereo 16 kHz WAVs as-is.
- **Custom Rev A PDM path.** The proposed pin map compiles with ESP-IDF PDM RX
  and RGB status output, but the schematic, mic select wiring, acoustic ports,
  and LED polarity still need board-level validation.

A corrupt-clip guard is already in place: `app_main.c` aborts on a failed SD
write and refuses to upload if the WAV header can't be patched on close. Caveat:
a close-failed clip stays in the card root, and the next boot's retry pass
cannot tell it from a healthy pending clip, so it will be attempted then —
inspect/remove such clips before rebooting if that matters.

## TODO (ROADMAP v1)

- [ ] Flash on hardware and tune the first real Wi-Fi/audio bring-up issues.
- [x] Hold-to-record button via direct GPIO (`BSP_PLUS_BUTTON_GPIO`, GPIO4).
- [x] Retry queue + rotate filenames (`rec_NNNN.wav`, uploaded clips → `/sdcard/sent/`).
- [ ] Make `wifi_connect` re-enterable (clean up netif/handlers on a failed join)
      so uploads can retry within a boot instead of latching off until reboot.
- [x] LCD/log status UI via ST7789/RGB state colors.
- [ ] Confirm whether 4-bit SDMMC is reliable, then consider `BSP_SD_WIDTH=4`.

## Sources

- Hardware target: [Waveshare ESP32-S3-LCD-1.54 / Touch-LCD-1.54 docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
  (same core features except touch).
- Pin map: Waveshare `ESP32-S3-LCD-1.54-Schematic.pdf` and
  [`waveshareteam/ESP32-S3-Touch-LCD-1.54`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)
  ESP-IDF BSP.
- Capture recipe: ESP-IDF `examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm`.
