---
type: Component
title: Server
description: Python FastAPI service that ingests audio, persists recordings, runs the provider pipeline, and renders the web UI.
resource: ../../server
tags: [server, fastapi, pipeline, sqlite]
timestamp: 2026-06-16T00:00:00+09:00
---
# Responsibilities

The server owns the end-to-end processing loop after a device uploads audio:

1. Accept multipart audio through the [recordings API](/apis/recordings-api.md).
2. Persist a [recording](/contracts/recording-data-model.md) row in SQLite and store the audio blob on local disk.
3. Run the provider-agnostic [pipeline](/contracts/pipeline-provider-contract.md) as a FastAPI background task.
4. Expose JSON polling, audio playback, web views, and Markdown export.

# Implementation Areas

| Path | Role |
|---|---|
| `server/open_plaud/api/app.py` | FastAPI app, routes, schemas, web templates, and upload handling. |
| `server/open_plaud/pipeline/orchestrator.py` | Stage runner for transcription, diarization, and summarization. |
| `server/open_plaud/store.py` | Durable SQLite store and serialization helpers. |
| `server/open_plaud/transcription/` | `none`, `openai_whisper`, and `whisper_cpp` providers. |
| `server/open_plaud/summarization/` | `none`, `claude`, and `ollama` providers. |
| `server/open_plaud/diarization.py` | `none`, `pyannote`, and `whisperx` providers. |
| `server/open_plaud/config.py` | `OP_` environment settings. |

# Defaults

The default pipeline providers are all `none`, so the server can start and test
without cloud credentials. Real transcription and summarization are selected
through environment-backed config.

# Citations

[1] [Server API implementation](../../server/open_plaud/api/app.py)
[2] [Pipeline orchestrator](../../server/open_plaud/pipeline/orchestrator.py)
[3] [Store implementation](../../server/open_plaud/store.py)
[4] [Architecture](../../docs/ARCHITECTURE.md)
