---
type: System
title: Open Plaud
description: Open reference stack for a Plaud-like recorder with DIY ESP32-S3 hardware, firmware, and a self-hosted transcription and summarization server.
resource: ../../README.md
tags: [product, architecture, recorder]
timestamp: 2026-06-16T00:00:00+09:00
---
# Purpose

`open-plaud` gives users custody of recordings and the AI pipeline. The stack
records audio on open hardware, uploads raw clips to a user-controlled server,
then runs configurable transcription, optional diarization, summarization, and
browsing/export.

# Architecture

The system keeps the recorder intentionally simple:

1. The [firmware](/components/firmware.md) records WAV files to microSD.
2. Sync uploads queued clips to the [recordings API](/apis/recordings-api.md).
3. The [server](/components/server.md) persists audio and metadata.
4. The [pipeline provider contract](/contracts/pipeline-provider-contract.md) selects transcription, diarization, and summarization providers by config.
5. Clients poll or browse the resulting [recording data model](/contracts/recording-data-model.md).

# Components

| Area | Role |
|---|---|
| [Firmware](/components/firmware.md) | ESP32-S3 recorder, local WAV storage, deferred Wi-Fi sync. |
| [Hardware](/components/hardware.md) | BOM, case, selected Waveshare LCD-1.54 target, custom PCB Rev A notes. |
| [Server](/components/server.md) | FastAPI ingest, storage, pipeline orchestration, web UI, Markdown export. |
| [Mobile iOS](/components/mobile-ios.md) | SwiftPM package for Apple Speech on-device transcription. |
| [Mac client](/components/mac-client.md) | Swift app/CLI client surface for local desktop workflows. |

# Invariants

* The device records and uploads; it does not transcribe or summarize.
* New AI capabilities should arrive as providers behind existing protocols.
* The default server configuration runs offline with `none` providers and no API keys.
* Hardware public-alpha status depends on bring-up evidence, not just source or build success.

# Citations

[1] [README](../../README.md)
[2] [Architecture](../../docs/ARCHITECTURE.md)
[3] [Roadmap](../../docs/ROADMAP.md)
