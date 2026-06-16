---
type: Workflow
title: Device Sync
description: Operational path for recording locally on the device and uploading queued clips to the server.
resource: ../../firmware/README.md
tags: [workflow, firmware, upload]
timestamp: 2026-06-16T00:00:00+09:00
---
# Trigger

The user presses the middle Sync button, or the device boots and drains the retry
queue if Wi-Fi is available.

# Steps

1. Scan pending `rec_NNNN.wav` files in the microSD root.
2. Connect to configured Wi-Fi, trying the last successful slot first.
3. Upload each queued clip, oldest first, to `POST {OP_SERVER}/v1/recordings`.
4. Move successful clips to `/sdcard/sent/`.
5. Leave failed clips in the root for the next Sync press or boot retry.

# Failure Behavior

Every network failure preserves the local clip. The device retries later rather
than deleting audio or blocking recording behind connectivity.

# Related Concepts

* [Firmware](/components/firmware.md)
* [Recordings API](/apis/recordings-api.md)
* [Recording data model](/contracts/recording-data-model.md)

# Citations

[1] [Firmware README](../../firmware/README.md)
[2] [Architecture data flow](../../docs/ARCHITECTURE.md)
