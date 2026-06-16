---
type: API
title: Recordings API
description: HTTP contract used by devices and clients to upload audio, poll processing state, fetch audio, and export recording notes.
resource: ../../server/open_plaud/api/app.py
tags: [api, fastapi, recordings, upload]
timestamp: 2026-06-16T00:00:00+09:00
---
# Endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/healthz` | Health check returning `{ "status": "ok" }`. |
| `POST` | `/v1/recordings` | Multipart audio upload under field `file`; returns a recording response immediately and schedules background processing. |
| `GET` | `/v1/recordings` | List recordings, newest first. |
| `GET` | `/v1/recordings/{recording_id}` | Fetch one recording response or `404` if missing. |
| `GET` | `/v1/recordings/{recording_id}/audio` | Stream stored audio using an allowlisted media type. |
| `GET` | `/v1/recordings/{recording_id}/export.md` | Export summary, action items, transcript, and error state as Markdown. |
| `GET` | `/` | Render recording list web UI. |
| `GET` | `/rec/{recording_id}` | Render recording detail web UI. |
| `POST` | `/rec/{recording_id}/reprocess` | Re-run background processing and redirect to the detail page. |

# Upload Contract

The firmware sync workflow posts audio like this:

```http
POST /v1/recordings
Content-Type: multipart/form-data

file=@rec_0001.wav
```

Accepted suffixes are `.wav`, `.flac`, `.m4a`, and `.mp3`. Accepted media types
are `audio/wav`, `audio/x-wav`, `audio/flac`, `audio/mpeg`, and `audio/mp4`.
Empty uploads return `400`; unsupported audio returns `415`; files larger than
`OP_UPLOAD_MAX_BYTES` return `413`.

# Response Shape

Responses serialize the [recording data model](/contracts/recording-data-model.md):

```json
{
  "id": "uuidhex",
  "status": "received",
  "filename": "rec_0001.wav",
  "size_bytes": 123456,
  "duration_seconds": 12.3,
  "created_at": "2026-06-16T00:00:00+00:00",
  "transcript": null,
  "summary": null,
  "error": null
}
```

# Processing Semantics

The upload response is intentionally fast. Processing happens in a background
task, so clients should poll `GET /v1/recordings/{id}` until `status` becomes
`done` or `failed`.

# Citations

[1] [API implementation](../../server/open_plaud/api/app.py)
[2] [Firmware device-server contract](../../firmware/README.md)
