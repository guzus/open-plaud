# Firmware — ESP-IDF recorder

The recorder firmware. ESP-IDF (C), no Arduino. Three top-edge buttons drive a
deferred-sync recorder:

- **Right** — tap to start recording a numbered WAV (`rec_NNNN.wav`) to microSD,
  tap again to stop. Stop **saves and queues** the clip; it does **not** upload
  (sync is deferred so you can record off Wi-Fi). Left **cancels/discards** while
  recording.
- **Middle (Sync)** — upload every queued clip, oldest first, to the server.
- **Left (Power)** — screen on/off when idle.

Boot drains the retry queue automatically if Wi-Fi is up. Uploaded clips move to
`/sdcard/sent/`; clips that fail to upload stay in the card root and are retried
on the next Sync press or boot. The LCD shows each button's current function
along the top edge, plus battery and microSD usage. See the signed-off UX in
[`docs/ui-prototype.html`](../docs/ui-prototype.html).

> **Status: validated on a Waveshare board (2026-06-16).** The end-to-end
> record → upload → cloud loop works, and the 3-button UX (toggle record,
> deferred sync, on-screen labels, battery + storage) was flashed and checked on
> hardware. Button mapping is confirmed (leftmost=GPIO0=power, middle=GPIO5=sync,
> rightmost=GPIO4=record). Remaining: the **battery divider** needs calibration —
> see "Validate on first hardware bring-up".

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
boot), a `button map: LEFT=GPIO0 … MIDDLE=GPIO5 … RIGHT=GPIO4 …` line, then
`ready — tap Right (GPIO4) to record`.

- **Tap Right** to start `rec_0001.wav`, `rec_0002.wav`, … (numbers are never
  reused — the root *and* `/sdcard/sent/` are scanned); **tap Right again** to
  stop. Stop saves and **queues** the clip — it is not uploaded here.
- **Tap Middle (Sync)** to upload the queue to `POST {server}/v1/recordings`,
  oldest first. Each success moves to `/sdcard/sent/`; on failure the clip stays
  in the SD root and Sync re-enters Wi-Fi setup on the next press (the next boot
  also retries).
- **Left** toggles the screen when idle, and **cancels** (discards) the current
  clip while recording.

A recording auto-stops after `OPEN_PLAUD_MAX_REC_SECONDS` (default 1800 s).

**Power:** to cut idle draw, audio is initialized only while a clip is recording
(torn down after the WAV closes) and Wi-Fi is started only for upload work and
stopped after each batch. The CPU still polls buttons in the ready loop; deep
sleep + button wake remains a follow-up. Battery % uses a calibrated 3:1 divider
(`power.c`), and the **Left** button blanks the screen to save the backlight.

## Layout

| File | Role |
|---|---|
| `main/board.h` | **pin maps** for Waveshare LCD-1.54 and custom PCB Rev A |
| `main/audio.c` | ES7210/TDM capture for Waveshare; direct PDM RX for Rev A |
| `main/button.c` | three top-edge buttons (record / sync / power) — polled inputs + pull-ups, raw level only; `app_main` debounces |
| `main/power.c` | battery percentage + charge status (ADC1 on GPIO1; divider ratio is a calibratable constant) |
| `main/sdcard.c` | mount microSD (FATFS over configurable 1-bit/4-bit SDMMC) + `sdcard_usage()` for the UI |
| `main/wav.c` | streaming 16-bit PCM WAV writer |
| `main/rec_store.c` | clip numbering (`rec_NNNN.wav`, never reused), `/sdcard/sent/` move, boot-time retry queue |
| `main/net_upload.c` | Wi-Fi STA + multipart upload to the server |
| `main/status.c` | Device UI: ST7789 LCD (per-button labels, state, battery + microSD usage; partial redraws keep capture unstalled) on Waveshare, RGB LED states on Rev A |
| `main/app_main.c` | orchestration: mount → boot retry → 3-button loop (toggle record, deferred sync, screen toggle) |
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
- **Button polarity / mapping — VERIFIED (2026-06-16).** Buttons are active-low
  (short to GND, idle high on the internal pull-up). Physical layout confirmed by
  pressing each and watching the screen: leftmost=GPIO0 (power/screen, cancel),
  middle=GPIO5 (sync), rightmost=GPIO4 (record/stop) — set in `board.h`. Note the
  leftmost button is GPIO0, the boot-strapping pin: holding it during power-on
  enters ROM download mode instead of running the app.
- **Toggle + cancel behavior.** Tap Right → it should start and keep recording
  after you let go (not stop on release). Tap Right again → stop + queue. Press
  Left mid-recording → clip discarded (no `rec_NNNN.wav` left behind). Confirm
  the on-screen `MM:SS` ticks and audio has no periodic dropouts (the timer is a
  partial redraw specifically to avoid stalling I2S capture).
- **Battery reading (`power.c`).** The divider ratio (`BAT_DIVIDER`, default 2.0),
  EN polarity, and charge-stat polarity are UNVERIFIED. With `CONFIG_LOG_DEFAULT_LEVEL`
  at debug you'll see `battery raw=… adc=…mV vbat=…mV`; compare `vbat` to a meter
  and adjust `BAT_DIVIDER`/`BAT_FULL_MV`/`BAT_EMPTY_MV`. An implausible reading
  shows `BAT --` rather than a wrong percentage.
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

- [x] Toggle record (tap to start/stop, no holding) via direct GPIO.
- [x] Three-button UX: Right=record/stop, Middle=Sync (deferred upload),
      Left=screen/cancel, with on-screen per-button labels.
- [x] Battery % + microSD usage on the LCD (incl. while recording).
- [x] Retry queue + rotate filenames (`rec_NNNN.wav`, uploaded clips → `/sdcard/sent/`).
- [x] Make `wifi_connect` re-enterable so uploads can retry within a boot.
- [x] Power down audio outside recording + stop Wi-Fi after upload (idle draw).
- [x] Calibrate the battery divider (3:1) so the % reads true (2026-06-16).
- [x] Flash the 3-button build; confirm button mapping on hardware (2026-06-16).
- [ ] Confirm whether 4-bit SDMMC is reliable, then consider `BSP_SD_WIDTH=4`.
- [ ] Optional Mac companion client (USB drive / serial / BLE) for off-Wi-Fi sync.

## Sources

- Hardware target: [Waveshare ESP32-S3-LCD-1.54 / Touch-LCD-1.54 docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
  (same core features except touch).
- Pin map: Waveshare `ESP32-S3-LCD-1.54-Schematic.pdf` and
  [`waveshareteam/ESP32-S3-Touch-LCD-1.54`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)
  ESP-IDF BSP.
- Capture recipe: ESP-IDF `examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm`.
