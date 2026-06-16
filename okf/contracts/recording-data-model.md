---
type: Contract
title: Recording Data Model
description: Shared recording, transcript, summary, and API response fields used by the server and clients.
resource: ../../server/open_plaud/types.py
tags: [data-model, api, sqlite]
timestamp: 2026-06-16T00:00:00+09:00
---
# Domain Objects

| Object | Fields |
|---|---|
| `AudioRef` | `path`, `filename`, `content_type`, `size_bytes`, `duration_seconds` |
| `TranscriptSegment` | `text`, `start`, `end`, `speaker` |
| `Transcript` | `text`, `language`, `segments` |
| `ActionItem` | `text`, `owner`, `due` |
| `Summary` | `summary`, `action_items` |
| `Recording` | `id`, `audio`, `status`, `created_at`, `transcript`, `diarization`, `summary`, `error` |

# Status Values

| Value | Meaning |
|---|---|
| `received` | Upload accepted and stored. |
| `transcribing` | Pipeline is producing transcript text and segments. |
| `diarizing` | Optional speaker-segment stage is running. |
| `summarizing` | Summary/action-item stage is running. |
| `done` | Pipeline finished successfully. |
| `failed` | A stage raised and `error` contains the exception type and message. |

# Persistence

SQLite stores flattened recording fields plus JSON columns for transcript
segments, diarization payloads, and summaries. The store uses WAL, a shared lock,
and per-write commits to preserve durability across restarts.

# API Projection

The [recordings API](/apis/recordings-api.md) exposes a Pydantic response model
derived from the domain object. `diarization` is persisted but is not returned
directly in the public response; speaker labels are merged into transcript
segments when diarization runs.

# Citations

[1] [Domain types](../../server/open_plaud/types.py)
[2] [Store implementation](../../server/open_plaud/store.py)
[3] [API response models](../../server/open_plaud/api/app.py)
