# Power & battery life

> # ⚠ EVERY NUMBER IN THIS FILE IS AN UNMEASURED ESTIMATE
>
> **No current has ever been measured on this board.** Not once. Every mA, every
> hour, and every battery-life row below is derived from the ESP32-S3 datasheet
> plus typical figures for the peripherals (ST7789 backlight, ES7210, microSD,
> Wi-Fi) — Waveshare publishes neither a current-draw spec nor the bundled cell's
> capacity. Treat these as the right *order of magnitude* and nothing more.
>
> **This matters beyond this document.** Endurance is the gap between this
> project and the devices it is measured against (Plaud: 30 h recording, 60-day
> standby). Every power-motivated engineering decision — sleep policy, screen
> blanking, capture format, transfer codec, sync cadence — is currently being
> argued from these guesses. Some of them are certainly wrong, and we cannot say
> which.
>
> ## The single highest-value next work session
>
> Put a multimeter in series with the cell (or a USB power meter inline) and
> record steady-state current in exactly these four states:
>
> | # | State | How to reproduce | Estimate to replace |
> |---|---|---|---|
> | 1 | **Ready, screen on** | Boot, idle at the ready screen | ~50 mA |
> | 2 | **Ready, screen off** | Idle, then tap **Left/Power** to blank the backlight | ~30 mA |
> | 3 | **Recording** | Tap **Right/Record**, let it run past the screen-blank timeout | ~85 mA |
> | 4 | **Wi-Fi sync** | Queue a clip, tap **Middle/Sync**, average over the upload | ~210 mA |
>
> Four numbers. They convert this entire document from planning fiction into an
> engineering budget, and they decide which optimizations are worth building.
> Until they exist, treat any "this saves battery" claim in this repo — including
> the ones below — as a hypothesis.
>
> Do **not** measure deep sleep by enabling it on this board without reading the
> deep-sleep section first: a wake does not restore USB-Serial/JTAG and there is
> no reset button, so it can strand the device un-reflashable.

## TL;DR

While *actively on*, the recorder is a **use-and-charge** device measured in
**hours**:

- **Idle, screen on:** ~3–8 h (depending on cell size)
- **Idle, screen off** (tap the **Left/Power** button to blank the backlight): ~6–14 h
- **Continuous recording:** ~2–5 h

**Deep sleep is implemented but ships DEFAULT OFF (opt-in)** on this board. It
works — after `OPEN_PLAUD_IDLE_SLEEP_S` (>0) of idle the device sleeps and wakes on
a Record press — *but* it powers down the
USB-Serial/JTAG and a deep-sleep **wake does NOT restore it** (only a full reset
does). This board has **no reset button**, so enabling deep sleep can strand the
device **un-reflashable + un-USB-recognizable until a battery power-cycle**. So it
ships **off** (`OPEN_PLAUD_IDLE_SLEEP_S = 0`) until a USJ-restore-on-wake fix lands;
set it `> 0` only if you accept that trap. (Its sleep *current* was never measured,
so the months-scale standby below is theoretical.) The screen still blanks while
recording (`OPEN_PLAUD_REC_SCREEN_S`, default 10 s) to drop the backlight.

On the Waveshare LCD-1.54 board, battery operation also depends on `BAT_EN`
(GPIO2): the physical PWR button gives temporary battery power, and firmware must
drive `BAT_EN` high to keep the board alive after the button is released. The
firmware holds it high at boot and enables GPIO deep-sleep hold so battery power
stays latched while unplugged.

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

## Deep sleep (implemented, ships DEFAULT OFF / opt-in)

After `OPEN_PLAUD_IDLE_SLEEP_S` (> 0) with no button press, the device enters
ESP32-S3 deep sleep and wakes on a **Record EXT1 button press**. Sync/Middle is
deliberately excluded so an accidental hold does not light the device while it is
sleeping on battery. Left/GPIO0 is also excluded because it is the boot strapping
pin.

**Why it ships OFF (`= 0`):** deep sleep powers down the USB-Serial/JTAG, and a
deep-sleep *wake does not restore it* — only a full reset does. This board has
**no reset button**, so a slept device is **un-reflashable and un-USB-recognizable
until you power-cycle the battery** (learned the hard way 2026-06-16). The wake
also skips Wi-Fi-retry + SD-autoformat for speed, but that doesn't touch the USB
problem. Until a **USJ-restore-on-wake fix** (e.g. `esp_restart()` on wake to force
a clean boot, carefully tested so it can't boot-loop on a board you can't reset) is
in and proven, leave it off. Other power savings (audio only while recording, Wi-Fi
off after upload, screen blank while recording) are always on regardless.

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
- **Battery power hold** — on the Waveshare LCD-1.54 board, GPIO2 / `BAT_EN` is
  driven high and held through deep sleep so unplugged battery operation survives
  releasing the physical PWR button.
- Deferred sync means the radio only comes up when **you** press Sync (or on boot
  if Wi-Fi is in range), not on every clip.

## Make this real (replace estimates with measurements)

This is the procedure for the four measurements called out at the top of this
file — it is the highest-value work session available on this project.

1. Read the **actual cell capacity** off the battery and pick the right table row.
2. Put a USB power meter (or a multimeter in series with the cell) on the board
   and record steady-state current in each of the four states: ready screen-on,
   ready screen-off, recording, and during a sync. Replace the table above.
3. With real per-state currents and capacity, recompute `life = capacity × 0.85 ÷ current`.
4. Only then re-litigate the codec/sleep tradeoffs. Until step 2 exists there is
   no evidence for which of them pays for itself — e.g. whether an on-device
   Opus encode (~70% of a core on ESP32 @240 MHz, unverified on the S3) beats
   the near-free ADPCM path on a radio that runs for seconds per clip.
5. Deep sleep standby is the *last* measurement, not the first, and only on a
   board you can recover — see the deep-sleep section above for why.

_Last updated 2026-08-01. Estimates only — **no current measured yet**._
