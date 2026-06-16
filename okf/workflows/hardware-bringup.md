---
type: Workflow
title: Hardware Bring-Up
description: Evidence path for validating the recorder hardware, firmware, case, and upload loop before public-alpha claims.
resource: ../../docs/BRINGUP.md
tags: [workflow, hardware, validation]
timestamp: 2026-06-16T00:00:00+09:00
---
# Acceptance Bar

The hardware reference should not be treated as public-alpha complete until
bring-up has concrete evidence:

* Serial logs from the flashed board.
* Recorded WAV samples.
* Successful upload responses from the server.
* Confirmation of button mapping, SD behavior, battery reading, and case fit.

# Mechanics

The bring-up workflow validates the [firmware](/components/firmware.md) against
real board behavior and confirms the [hardware](/components/hardware.md) details
that cannot be proven from source code alone.

# Risk Areas

* Audio init path and real I2S behavior.
* FATFS rename into `/sdcard/sent/`.
* Button debounce timing.
* Battery divider and charge-state calibration.
* Deep-sleep wake behavior and USB-Serial/JTAG recovery.
* SDMMC width reliability.

# Citations

[1] [Bring-up checklist](../../docs/BRINGUP.md)
[2] [Firmware validation notes](../../firmware/README.md)
[3] [Roadmap](../../docs/ROADMAP.md)
