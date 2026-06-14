# server-go — ingest + transcription server (Go, stdlib-only)

A minimal, device-compatible drop-in for the Python `../server`. It receives the
recorder's WAV uploads, **transcribes them via Deepgram** (when
`DEEPGRAM_API_KEY` is set), and exposes both the audio and the transcript.
Summarization is still out of scope (that lives in `../server`, in Python). A
cheap, single-binary cloud endpoint on Railway for the PoC.

Live PoC: **https://ingest-production-4fd2.up.railway.app**

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET`  | `/healthz` | `{"status":"ok"}` |
| `POST` | `/v1/recordings` | multipart field **`file`** → `200 {Recording}` |
| `GET`  | `/v1/recordings` | list, newest first |
| `GET`  | `/v1/recordings/{id}` | one recording |
| `GET`  | `/v1/recordings/{id}/audio` | download the stored blob (Range-enabled) |
| `GET`  | `/` | tiny HTML list for eyeballing uploads |

`Recording` JSON matches the Python `RecordingResponse` field-for-field
(`id, status, filename, size_bytes, created_at, transcript, summary, error`).
`status` walks `received → transcribing → done` (or `failed`); `transcript` is
`{text, language, segments}` once Deepgram returns; `summary` stays `null`. With
no `DEEPGRAM_API_KEY` set, uploads stay `received` with a `null` transcript. The
device only checks for a 2xx on `POST`, so this stays compatible with
`firmware/main/net_upload.c`.

## Run locally

```bash
cd server-go
OP_STORAGE_DIR=./data PORT=8000 go run .
# POST a file:
curl -F "file=@sample.wav;type=audio/wav" http://127.0.0.1:8000/v1/recordings
```

Config (env): `PORT` (default 8000), `OP_STORAGE_DIR` (default `/data`),
`OP_UPLOAD_MAX_BYTES` (default 200 MiB), `OP_AUTH_TOKEN` (default empty = open),
`DEEPGRAM_API_KEY` (default empty = transcription off), `DEEPGRAM_MODEL`
(default `nova-3`). No external dependencies, no database — each upload is a blob
`{id}{ext}` + sidecar `{id}.json` in the storage dir.

## Transcription (Deepgram)

When `DEEPGRAM_API_KEY` is set, each upload is transcribed asynchronously: the
`POST` returns `received` immediately and a background goroutine calls Deepgram's
pre-recorded API (`/v1/listen`, model `nova-3`, `smart_format`, `detect_language`
— so Korean/English recordings are handled automatically), then persists the
transcript and flips status to `done`. Poll `GET /v1/recordings/{id}` for it.
On restart, any recording still `received`/`transcribing` is re-enqueued.

The key is a **secret** — it lives only as a Railway service variable, never in
the repo:

```bash
railway variables --set "DEEPGRAM_API_KEY=<your-key>"   # redeploys with transcription on
```

## Security

By default the server is **open**: `GET /v1/recordings` and `/` list and link
*every* recording for download to anyone who has the URL. Fine for a throwaway
PoC; **not** fine once real conversations are uploaded.

Set `OP_AUTH_TOKEN` to require a shared secret on every route except `/healthz`:

```bash
railway variables --set "OP_AUTH_TOKEN=$(openssl rand -hex 24)"   # redeploys
```

Then callers must send `Authorization: Bearer <token>`. The device side is one
line in `firmware/main/net_upload.c` (a Kconfig string + `esp_http_client_set_header`):

```c
esp_http_client_set_header(client, "Authorization", "Bearer " CONFIG_OPEN_PLAUD_AUTH_TOKEN);
```

## Deploy to Railway

Already deployed (project `open-plaud`, service `ingest`, persistent volume at
`/data`). To redeploy after changes:

```bash
cd server-go
railway up        # builds the Dockerfile, pushes, deploys
```

Notes:
- Railway does not inject `PORT` for this service, so the app uses 8000 (matching
  the Dockerfile `EXPOSE`/`ENV`). The `/data` volume keeps recordings across
  redeploys.
- The Dockerfile is multi-stage → distroless static; the image is just the ~9 MB
  static binary.

## Pointing the device at this server

The recorder is configured in `firmware` via `idf.py menuconfig` →
`OPEN_PLAUD_SERVER_URL`.

- **LAN (no firmware change):** `http://<your-mac-ip>:8000` running `../server`
  or this server locally. Best for first-board bring-up — plain HTTP, fewest
  variables to debug.
- **Railway (HTTPS — needs a firmware change):** `https://ingest-production-4fd2.up.railway.app`.
  `esp_http_client` in `firmware/main/net_upload.c` is created with **no TLS cert
  bundle**, so an HTTPS upload will fail the handshake. To enable it:

  ```c
  // firmware/main/net_upload.c
  #include "esp_crt_bundle.h"
  ...
  esp_http_client_config_t cfg = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = CONFIG_OPEN_PLAUD_UPLOAD_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,   // verify *.up.railway.app
  };
  ```

  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` is on by default in ESP-IDF, so this is the
  only code change required. (Adds ~64 KB of root CAs to the image.)
