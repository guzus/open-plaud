# First-board bring-up checklist

This is the public-alpha gate for the Waveshare ESP32-S3-LCD-1.54 reference
recorder. The server and provider contracts can be tested on a laptop, but the
project should not claim working hardware until this checklist has real board
evidence attached.

## What this proves

- The selected board boots the open-plaud firmware.
- The firmware captures a valid 16 kHz, 16-bit, stereo PCM WAV to microSD.
- The PLUS button starts and stops recording with the expected polarity.
- The device uploads the WAV as multipart field `file` to
  `POST /v1/recordings`.
- Successful uploads move clips from `/sdcard/rec_NNNN.wav` to
  `/sdcard/sent/rec_NNNN.wav`.
- Failed uploads stay on SD and can recover later in the same boot or on the
  next boot.

## Software preflight

Run these before flashing hardware:

```bash
cd server
uv run ruff check .
uv run mypy .
uv run pytest -q

cd ../mobile/ios
swift run open-plaud-mobile-verify

cd ../../firmware
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -B build-rev-a -DSDKCONFIG=build-rev-a/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rev_a.defaults" build
```

Local API smoke test, without API keys:

```bash
cd server
tmpdir=$(mktemp -d /tmp/open-plaud-smoke.XXXXXX)
OP_STORAGE_DIR="$tmpdir/storage" uv run uvicorn open_plaud.api.app:app \
  --host 127.0.0.1 --port 8765
```

In another shell, upload a known-good WAV and confirm `received` changes to
`done` when polled:

```bash
curl -F "file=@sample.wav;type=audio/wav" \
  http://127.0.0.1:8765/v1/recordings
```

## Hardware setup

- Waveshare ESP32-S3-LCD-1.54, SKU 33867, with bundled battery.
- FAT32 microSD card.
- USB-C cable that carries data.
- Local server reachable from the same Wi-Fi network as the ESP32-S3.
- ESP-IDF v5.4.x sourced in the flashing shell.

Firmware config:

```bash
cd firmware
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py menuconfig
```

Set:

- `OPEN_PLAUD_BOARD_WAVESHARE_LCD154`
- `OPEN_PLAUD_WIFI_SSID`
- `OPEN_PLAUD_WIFI_PASSWORD`
- `OPEN_PLAUD_SERVER_URL`, for example `http://192.168.1.50:8000`
- `OPEN_PLAUD_SDMMC_WIDTH=1` for first bring-up

Flash and monitor:

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Bring-up pass

Record this evidence for the first public alpha:

| Gate | Pass evidence |
|---|---|
| Boot | Serial log shows SD card mounted, audio ready, and ready prompt. |
| Button | Holding PLUS starts recording; releasing PLUS stops it. |
| WAV integrity | `rec_0001.wav` plays on a desktop and has a valid duration/header. |
| Upload | Server returns 2xx from `POST /v1/recordings`; poll reaches `done`. |
| Move-to-sent | Uploaded clip is absent from SD root and present in `/sdcard/sent/`. |
| Retry on server down | With server stopped, clip stays in root; after server restarts, a later upload attempt succeeds in the same boot. |
| Retry on reboot | A queued root clip uploads oldest-first after reboot. |
| Max length | Held button auto-stops at `OPEN_PLAUD_MAX_REC_SECONDS`. |
| Display/status | LCD/status state matches boot, ready, recording, syncing, done, error. |
| Case fit | USB, TF, button, mic ports, screen, and battery all clear the printed case. |

## Known first-board risks

- ES7210/TDM capture may double-configure the RX path depending on
  `esp_codec_dev` behavior. If `esp_codec_dev_open` or `i2s_channel_enable`
  fails, remove the duplicate enable path and rebuild.
- PLUS button polarity is still an assumption. If recording starts on boot or
  never starts when held, flip `BSP_BTN_ACTIVE_LEVEL`.
- FATFS rename into `/sdcard/sent` should work on one volume, but the first
  board must confirm it with real media.
- Upload retries are not idempotent yet. If the server stores a clip but the
  device misses the response, a later retry can create a duplicate recording.
  The first hardware retry tests should watch for this before public demos.
- Keep SDMMC in 1-bit mode until basic recording and upload pass.
- Stereo upload is correct but doubles file size versus mono. Down-mix only
  after confirming whether two-channel capture helps real meeting audio.

## Public-alpha release gate

Do not call the hardware reference validated until the bring-up pass has:

- Serial monitor log.
- One short raw WAV sample.
- Server upload response and `GET /v1/recordings/{id}` response.
- Photo of board and case fit.
- Notes for any code/config change made during bring-up.

Store those artifacts under `docs/bringup/YYYY-MM-DD-waveshare-lcd154/` and link
them from this file.
