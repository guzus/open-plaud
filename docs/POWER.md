# Power & battery life

> **These are engineering ESTIMATES, not measurements.** No current was measured
> on the board yet, and Waveshare does not publish a current-draw spec or the
> bundled cell's capacity. The numbers below come from the ESP32-S3 datasheet
> plus typical figures for the peripherals (ST7789 backlight, ES7210, microSD,
> Wi-Fi). Treat them as the right *order of magnitude* and a planning tool, not a
> guarantee. See "Make this real" at the bottom to replace them with measurements.

## TL;DR

While *actively on*, the recorder is a **use-and-charge** device measured in
**hours**:

- **Idle, screen on:** ~3–8 h (depending on cell size)
- **Idle, screen off** (tap the **Left/Power** button to blank the backlight): ~6–14 h
- **Continuous recording:** ~2–5 h

But it no longer *stays* actively on: **deep sleep is implemented** — after an idle
timeout (`OPEN_PLAUD_IDLE_SLEEP_S`, default 60 s) the device sleeps and wakes
(reboots, ~2 s) on a **Record or Sync** press. Sleep/wake is **verified on
hardware**; the actual sleep *current* is **not yet measured**, so the
months-scale standby below is **expected, not confirmed**. The screen also blanks
while recording (`OPEN_PLAUD_REC_SCREEN_S`, default 10 s) to drop the backlight.

Sync (Wi-Fi upload) is the heaviest state but only runs for seconds per clip, so
it barely affects daily life.

## Assumptions

| Input | Value used | Notes |
|---|---|---|
| Cell voltage | 3.7 V nominal LiPo | Confirmed on HW: ADC reads ~1.37 V through a 3:1 divider → ~4.1 V charged. |
| Cell capacity | **unknown — parameterized** | Waveshare doesn't publish it. Read it off your cell (printed like `3.7V 300mAh`) and use the matching row. Tables cover 200 / 300 / 500 mAh. |
| Usable fraction | 0.85 | LiPo usable charge to cutoff × regulator/path loss. |

## Estimated current per state

Backlight is assumed on except where noted; Wi-Fi is **off** except during sync;
audio is **off** except during recording (the firmware enforces both — see below).

| State | Estimate | Range | What's drawing |
|---|---|---|---|
| Ready, **screen on** | **~50 mA** | 40–70 | ESP32-S3 active radio-off (~30) + LCD backlight (~20) |
| Ready, **screen off** | **~30 mA** | 25–45 | ESP32-S3 active radio-off only (backlight blanked) |
| **Recording** | **~85 mA** | 70–120 | SoC (~35) + backlight (~20) + ES7210 + mics (~12) + microSD writes (~20) |
| **Syncing** (Wi-Fi upload) | **~210 mA avg** | 150–300 peak | SoC + backlight + Wi-Fi TX (~150 avg, bursts to ~340). Lasts only seconds/clip. |
| Deep sleep *(implemented; draw unmeasured)* | **~0.02 mA** chip | 0.01–0.3 chip | RTC + EXT1 button wake only; CPU/radio/peripherals off. **Board** regulator + LCD-controller quiescent likely dominate — measure. |

Basis: ESP32-S3 active current with the radio off is ~30–45 mA (Espressif
datasheet); the ready loop `vTaskDelay(20 ms)`s between button polls so the CPU
is mostly coasting. Wi-Fi TX peaks ~250–340 mA on the S3. The backlight LED is
the single biggest *idle* load, which is why the Power button blanks it.

## Estimated battery life (hours), by cell capacity

`life = capacity_mAh × 0.85 ÷ current_mA`

| State | 200 mAh | 300 mAh | 500 mAh |
|---|---|---|---|
| Ready, screen on (~50 mA) | 3.4 h | 5.1 h | 8.5 h |
| Ready, screen off (~30 mA) | 5.7 h | 8.5 h | 14.2 h |
| Continuous recording (~85 mA) | 2.0 h | 3.0 h | 5.0 h |
| Syncing, continuous (~210 mA)\* | 0.8 h | 1.2 h | 2.0 h |
| Deep sleep standby (~0.02 mA)† | ~350 d | ~530 d | ~880 d |

\* Reference only — a real sync is seconds per clip, not continuous.
† Chip-only estimate (board sleep draw not yet measured — likely higher); LiPo
self-discharge caps useful standby at a few months regardless.

## Representative scenarios (300 mAh cell, ~255 mAh usable)

- **A recording session:** ~3 h of continuous capture on a charge.
- **"Pocket day" — 20 min recording + screen-off otherwise + a few syncs:**
  ~28 mAh recording + ~5 mAh sync leaves ~220 mAh for idle → **~7–8 h** of
  screen-off standby. So: comfortably a half-day of light use; **plan to charge
  daily** (or between sessions).
- **Leave it on the desk, screen on, doing nothing:** dead in **~5 h**. Blank the
  screen (Left button) to roughly double idle endurance.

## Deep sleep (implemented)

After `OPEN_PLAUD_IDLE_SLEEP_S` (default 60 s) with no button press, the device
enters ESP32-S3 deep sleep and wakes on an **EXT1 button press**. Verified on
hardware (2026-06-16): it sleeps, **stays** asleep (no spurious wake), wakes on
**Record (GPIO4)** and **Sync (GPIO5)**, and does **not** wake on **Left
(GPIO0)** — GPIO0 is the boot strapping pin, so it is deliberately excluded (held
low at the wake-reset would drop the chip into ROM download mode). Wake is a full
reboot (~2 s back to ready); the boot path skips the Wi-Fi retry and SD
autoformat on a wake so it stays fast and safe. (A nice side-effect: the
USB-Serial/JTAG port disappears while asleep — handy confirmation it really slept.)

**Still to measure:** the actual sleep *current*. The ESP32-S3 core sleeps to
~tens of µA, but the **board** draw (LDO/charger quiescent, LCD controller) is
unmeasured and likely dominates — put a meter on the cell in deep sleep to turn
the "months" estimate into a real number. Also confirm the Waveshare
**power-latch / charge-path** keeps the board powered from the cell while asleep.

## What the firmware already does for power

- **Audio is powered only while recording** — `audio_init()` in `record_clip()`,
  `audio_deinit()` after the WAV closes (not held from boot).
- **Wi-Fi is stopped after every upload batch** — `net_upload_shutdown()` after
  boot-retry and after each Sync (driver stays initialized for fast reconnect).
- **Screen blanking** — the Left/Power button turns the LCD backlight off/on; the
  backlight is the dominant idle load.
- Deferred sync means the radio only comes up when **you** press Sync (or on boot
  if Wi-Fi is in range), not on every clip.

## Make this real (replace estimates with measurements)

1. Read the **actual cell capacity** off the battery and pick the right table row.
2. Put a USB power meter (or a multimeter in series with the cell) on the board
   and record steady-state current in each state: ready screen-on, ready
   screen-off, recording, and during a sync. Replace the table above.
3. With real per-state currents and capacity, recompute `life = capacity × 0.85 ÷ current`.
4. Then implement deep sleep and measure standby draw to confirm the months-scale
   number.

_Last updated 2026-06-16. Estimates only — no current measured yet._
