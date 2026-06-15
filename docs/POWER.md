# Power & battery life

> **These are engineering ESTIMATES, not measurements.** No current was measured
> on the board yet, and Waveshare does not publish a current-draw spec or the
> bundled cell's capacity. The numbers below come from the ESP32-S3 datasheet
> plus typical figures for the peripherals (ST7789 backlight, ES7210, microSD,
> Wi-Fi). Treat them as the right *order of magnitude* and a planning tool, not a
> guarantee. See "Make this real" at the bottom to replace them with measurements.

## TL;DR

With the current firmware (no deep sleep yet), the recorder is a **use-and-charge**
device measured in **hours**, not days:

- **Idle, screen on:** ~3–8 h (depending on cell size)
- **Idle, screen off** (tap the **Left/Power** button to blank the backlight): ~6–14 h
- **Continuous recording:** ~2–5 h
- **Multi-day standby** is **not** possible until **deep sleep** is added (the one
  big remaining power win — see below). With it, standby jumps to **months**.

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
| Deep sleep *(not yet implemented)* | **~0.02 mA** | 0.01–0.3 | RTC + GPIO wake only; radio/CPU/peripherals off |

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
† Once implemented; in practice LiPo self-discharge caps useful standby at a few
months regardless.

## Representative scenarios (300 mAh cell, ~255 mAh usable)

- **A recording session:** ~3 h of continuous capture on a charge.
- **"Pocket day" — 20 min recording + screen-off otherwise + a few syncs:**
  ~28 mAh recording + ~5 mAh sync leaves ~220 mAh for idle → **~7–8 h** of
  screen-off standby. So: comfortably a half-day of light use; **plan to charge
  daily** (or between sessions).
- **Leave it on the desk, screen on, doing nothing:** dead in **~5 h**. Blank the
  screen (Left button) to roughly double idle endurance.

## The deep-sleep opportunity (biggest remaining win)

The firmware currently keeps the CPU awake in the ready loop polling buttons
(~30–50 mA). ESP32-S3 **deep sleep** with a GPIO/EXT1 wake on the Record button
would cut ready draw to **tens of microamps** — turning "hours" of standby into
**months**. This is the single highest-impact power change and is tracked as a
TODO. It needs validating against:

- the Waveshare battery **power-latch / charge-path** behavior (does the board
  stay powered from the cell in deep sleep?), and
- a **wake source** on the Record button (note the Left button is GPIO0, the
  strapping pin — prefer the Record button GPIO4 for wake), redrawing the LCD on
  wake.

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
