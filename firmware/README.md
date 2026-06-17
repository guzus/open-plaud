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
> rightmost=GPIO4=record). Battery hold/readout and Railway HTTPS sync are
> validated; sync shows active file progress and skips empty corrupt clips.

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

To build the CoreBluetooth companion firmware profile:

```bash
idf.py -B build-ble -DSDKCONFIG=build-ble/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble.defaults" build
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
  oldest first. The screen shows active-file percent while a clip is uploading.
  Each success moves to `/sdcard/sent/`; on failure the clip stays in the SD root
  and Sync re-enters Wi-Fi setup on the next press (the next boot also retries).
  Empty/header-only corrupt WAVs are discarded so one bad clip cannot block the
  queue.
- **Left** toggles the screen when idle, and **cancels** (discards) the current
  clip while recording.

A recording auto-stops after `OPEN_PLAUD_MAX_REC_SECONDS` (default 1800 s).

**Power:** to cut draw, audio is initialized only while a clip is recording (torn
down after the WAV closes), Wi-Fi is started only for upload work and stopped
after each batch, the screen blanks while recording (`OPEN_PLAUD_REC_SCREEN_S`,
default 10 s). **Deep sleep** is implemented (`OPEN_PLAUD_IDLE_SLEEP_S`) but ships
**OFF by default**: on this board it powers down the USB-Serial/JTAG and a wake
doesn't restore it, and there's no reset button — so enabling it can strand the
device un-reflashable + un-USB-recognizable until a battery power-cycle. On
battery, GPIO2 / `BAT_EN` is held high so the board stays alive after the physical
PWR button is released. Battery % uses a calibrated 3:1 divider (`power.c`); the
**Left** button blanks the screen on demand. See [`docs/POWER.md`](../docs/POWER.md)
for estimated battery life (~3–8 h active) and the deep-sleep caveat.
When the screen is off on battery, Middle/Sync is ignored instead of turning the
screen on; while charging, it may wake the screen and sync.

## Layout

| File | Role |
|---|---|
| `main/board.h` | **pin maps** for Waveshare LCD-1.54 and custom PCB Rev A |
| `main/audio.c` | ES7210/TDM capture for Waveshare; direct PDM RX for Rev A |
| `main/button.c` | three top-edge buttons (record / sync / power) — polled inputs + pull-ups, raw level only; `app_main` debounces |
| `main/power.c` | battery percentage + charge status (ADC1 on GPIO1; divider ratio is a calibratable constant) |
| `main/sdcard.c` | mount microSD (FATFS over configurable 1-bit/4-bit SDMMC) + `sdcard_usage()` for the UI |
| `main/wav.c` | streaming 16-bit PCM WAV writer |
| `main/rec_store.c` | clip numbering (`rec_NNNN.wav`, never reused), `/sdcard/sent/` move, boot-time retry queue, empty-clip discard |
| `main/net_upload.c` | Wi-Fi STA + multipart upload; runtime BLE/NVS credentials override menuconfig; legacy stereo WAVs stream as mono |
| `main/ble_companion.c` | optional CoreBluetooth companion GATT service for setup, status, sync commands, and chunked clip transfer |
| `main/ble_audio.c` | IMA ADPCM block encoder for compact BLE clip chunks |
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
  file=@rec_0001.wav            # 16 kHz, 16-bit, mono PCM WAV
→ 200 { "id": "...", "status": "received", "filename": "rec_0001.wav", "size_bytes": ... }
       # the server transcribes/summarizes in the background; poll GET for the result
```

New recordings are stored as mono WAVs. Older stereo WAVs already queued on the
SD card are downmixed in the upload stream without rewriting the original file,
then moved to `/sdcard/sent/` after a 2xx response.

Optional Wi-Fi compression (`OPEN_PLAUD_COMPRESS_TRANSFERS`) prefers Ogg Opus
when `OPEN_PLAUD_OPUS_TRANSFERS` is enabled. The firmware encodes a temporary
`.opus` file on SD, uploads it as `audio/ogg`, and deletes the temp file after
the request. If Opus encoding fails, uploads fall back to the Open Plaud ADPCM
container (`application/vnd.openplaud.adpcm`). BLE chunk transfer still uses the
IMA ADPCM compression family for compact iPhone pulls.

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
- `OPEN_PLAUD_BLE_COMPANION` (requires ESP-IDF Bluetooth/NimBLE enabled)
- `OPEN_PLAUD_COMPRESS_TRANSFERS`
- `OPEN_PLAUD_OPUS_TRANSFERS` / `OPEN_PLAUD_OPUS_BITRATE` (default 24 kbps mono voice)
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
- **Battery reading (`power.c`).** The divider ratio is calibrated to 3:1 on the
  Waveshare battery SKU. If the pack changes, compare `vbat` to a meter and
  adjust `BAT_DIVIDER`/`BAT_FULL_MV`/`BAT_EMPTY_MV`. An implausible reading shows
  `BAT --` rather than a wrong percentage.
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
- **Transcription cost:** device WAVs are mono. The capture backend still reads
  the board-native stereo mic stream and averages left/right samples before
  writing/uploading.
- **Custom Rev A PDM path.** The proposed pin map compiles with ESP-IDF PDM RX
  and RGB status output, but the schematic, mic select wiring, acoustic ports,
  and LED polarity still need board-level validation.

A corrupt-clip guard is already in place: `app_main.c` aborts on a failed SD
write and refuses to keep clips with no captured audio; `rec_store.c` also
discards empty/header-only WAVs during retry so they do not block later clips.

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
- [x] Deep sleep + screen-off-while-recording; wake on Record only (EXT1),
      initially verified on hardware (2026-06-16) — but shipped OFF by default (it strands
      USB/reflash on this reset-button-less board; see docs/POWER.md).
- [ ] USJ-restore-on-wake fix (e.g. esp_restart on wake) so deep sleep can be
      re-enabled safely; then measure real sleep current on the cell.
- [ ] Confirm whether 4-bit SDMMC is reliable, then consider `BSP_SD_WIDTH=4`.
- [ ] Optional Mac companion client (USB drive / serial / BLE) for off-Wi-Fi sync.

## Sources

- Hardware target: [Waveshare ESP32-S3-LCD-1.54 / Touch-LCD-1.54 docs](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
  (same core features except touch).
- Pin map: Waveshare `ESP32-S3-LCD-1.54-Schematic.pdf` and
  [`waveshareteam/ESP32-S3-Touch-LCD-1.54`](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)
  ESP-IDF BSP.
- Capture recipe: ESP-IDF `examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm`.
