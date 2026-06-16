---
type: Contract
title: Pipeline Provider Contract
description: Stable extension pattern for transcription, diarization, and summarization providers in the server pipeline.
resource: ../../server/open_plaud/pipeline/orchestrator.py
tags: [pipeline, providers, extension]
timestamp: 2026-06-16T00:00:00+09:00
---
# Core Rule

Pipeline stages are selected by name from config and resolved through provider
registries. The orchestrator depends on protocols and registry factories, not
provider implementations.

# Stages

| Stage | Protocol method | Built-in provider names |
|---|---|---|
| Transcription | `transcribe(audio, opts) -> Transcript` | `none`, `openai_whisper`, `whisper_cpp` |
| Diarization | `diarize(audio) -> object` | `none`, `pyannote`, `whisperx` |
| Summarization | `summarize(transcript, instructions=None) -> Summary` | `none`, `claude`, `ollama` |

# Status Transitions

The orchestrator updates a [recording](/contracts/recording-data-model.md)
through these statuses:

```text
received -> transcribing -> diarizing -> summarizing -> done
                                      \-> failed
```

`diarizing` is only used when diarization is requested. `summarizing` is only
used when a transcript with text exists and summarization is enabled.

# Extension Guidance

* Add a provider class that satisfies the stage protocol.
* Give it a unique `name`.
* Register it through the stage registry.
* Select it through `OP_PIPELINE__TRANSCRIPTION`, `OP_PIPELINE__DIARIZATION`, or `OP_PIPELINE__SUMMARIZATION`.
* Avoid changing the orchestrator unless the stage contract itself is wrong.

# Citations

[1] [Pipeline orchestrator](../../server/open_plaud/pipeline/orchestrator.py)
[2] [Transcription base](../../server/open_plaud/transcription/base.py)
[3] [Summarization base](../../server/open_plaud/summarization/base.py)
[4] [Diarization providers](../../server/open_plaud/diarization.py)
[5] [Architecture](../../docs/ARCHITECTURE.md)
