---
type: Component
title: Mobile iOS
description: SwiftPM package for on-device iOS transcription using Apple Speech recognition.
resource: ../../mobile/ios
tags: [ios, mobile, transcription, on-device]
timestamp: 2026-06-16T00:00:00+09:00
---
# Role

The iOS package provides an on-device transcription target for local mobile
workflows. It uses Apple Speech with `requiresOnDeviceRecognition = true` and
returns transcript text, language, timing segments, and engine metadata.

# Boundaries

The package is a transcription backend target, not a replacement for the
[server](/components/server.md) ingest and persistence loop. A bundled Core ML or
TFLite Whisper engine remains optional future work.

# Citations

[1] [Mobile iOS README](../../mobile/ios/README.md)
[2] [Swift source](../../mobile/ios/Sources/OpenPlaudMobileTranscription/OpenPlaudMobileTranscription.swift)
[3] [Architecture](../../docs/ARCHITECTURE.md)
