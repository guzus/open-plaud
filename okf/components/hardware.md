---
type: Component
title: Hardware
description: Physical recorder reference covering the Waveshare ESP32-S3-LCD-1.54 target, case files, BOM, and custom PCB Rev A planning.
resource: ../../hardware
tags: [hardware, bom, case, pcb]
timestamp: 2026-06-16T00:00:00+09:00
---
# Current Target

The v0 hardware target is the Waveshare ESP32-S3-LCD-1.54 with battery. It
bundles the recorder-critical pieces for the first public-alpha path: dual mic
plus ES7210 ADC, battery charging, TF/microSD, Wi-Fi, and a small LCD.

# Repository Areas

| Path | Contents |
|---|---|
| `hardware/bom/` | Shopping notes and Rev A BOM CSV. |
| `hardware/case/` | Parametric case generators, notes, and STL output. |
| `docs/CUSTOM_PCB_REV_A.md` | Pilot PCB architecture and assumptions. |
| `docs/HARDWARE_DECISION.md` | Hardware selection rationale. |
| `docs/POWER.md` | Power behavior and battery-life estimates. |

# Dependencies

Hardware progress controls the [firmware](/components/firmware.md) validation
bar. Source builds and local tests are not enough to call hardware public-alpha
complete without bring-up evidence.

# Citations

[1] [README hardware status](../../README.md)
[2] [Roadmap](../../docs/ROADMAP.md)
[3] [Firmware README](../../firmware/README.md)
