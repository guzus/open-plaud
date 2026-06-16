---
type: Component
title: Firmware
description: ESP-IDF firmware that records numbered WAV clips to microSD and uploads queued clips to the server on demand.
resource: ../../firmware
tags: [firmware, esp32-s3, recorder, sync]
timestamp: 2026-06-16T00:00:00+09:00
---
# Responsibilities

The firmware implements the device loop for the selected Waveshare ESP32-S3-LCD-1.54 target:

* Record `rec_NNNN.wav` clips to microSD.
* Keep uploaded clips under `/sdcard/sent/` and leave failed uploads queued.
* Upload queued clips to the [recordings API](/apis/recordings-api.md) only when sync runs.
* Show button labels, battery, microSD usage, and recorder state on the LCD.
* Support the custom PCB Rev A compile profile without forking the core server contract.

# User Controls

| Button | Idle behavior | Recording behavior |
|---|---|---|
| Left | Screen/power toggle | Cancel and discard current clip |
| Middle | Sync queued clips | No primary recording action |
| Right | Start recording | Stop, save, and queue clip |

# Key Files

| File | Role |
|---|---|
| `firmware/main/app_main.c` | Main boot, retry, record, sync, and button loop. |
| `firmware/main/audio.c` | ES7210/TDM capture for Waveshare and PDM RX for Rev A. |
| `firmware/main/net_upload.c` | Wi-Fi STA and multipart upload. |
| `firmware/main/rec_store.c` | Clip numbering, queueing, and sent-file moves. |
| `firmware/main/status.c` | LCD/RGB status output. |
| `firmware/main/board.h` | Board pin maps. |

# Operational Notes

Deep sleep exists but ships off by default because the reset-button-less
Waveshare board can lose USB-Serial/JTAG after wake. Battery divider calibration
and board-level validation remain tied to the hardware bring-up workflow.

# Citations

[1] [Firmware README](../../firmware/README.md)
[2] [Architecture](../../docs/ARCHITECTURE.md)
[3] [Bring-up checklist](../../docs/BRINGUP.md)
