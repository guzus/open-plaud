# open-plaud

An open-source, self-hostable alternative to AI voice recorders like Plaud — DIY hardware, a pluggable transcription pipeline, and AI summarization you control.

No per-minute limits. No forced cloud lock-in. No subscription. You own the recordings and the pipeline.

## Why

Commercial AI recorders ship cheap hardware and monetize a cloud subscription that caps your transcription minutes and processes your private audio on someone else's servers. `open-plaud` inverts that: the value lives in an open pipeline you run yourself.

| | Plaud (commercial) | open-plaud |
|---|---|---|
| Hardware | Closed, ~$159+ | Open, ~₩40–70k DIY |
| Transcription | Cloud-only, minute-capped | Pluggable: cloud now, local later |
| Summarization | Their templates | Any LLM (Claude / GPT / local) |
| Recordings | Their cloud | Your storage |
| Cost model | Subscription | Pay-per-use or free (local) |

## Repository layout

```
open-plaud/
├── firmware/      ESP32-S3 firmware (ESP-IDF) — record, sync over Wi-Fi
├── hardware/      BOM, 3D-print case files, wiring diagrams
├── server/        Python pipeline: ingest → transcribe → diarize → summarize
├── docs/          Build guide, architecture, API reference
└── scripts/       Dev/setup helpers
```

## Status

The server loop works end to end: upload → transcribe → summarize → browse. Cloud
transcription (OpenAI Whisper) and summarization (Claude) providers are implemented
behind the pipeline's protocols and selected by config; recordings persist in SQLite
and survive restarts; a small web UI lists, plays, and exports them. The default
providers are `none` (no key required) so the server runs offline out of the box.
On-device (desktop/mobile) inference can be added later as additional providers
without touching the rest of the pipeline. See `docs/ARCHITECTURE.md` and `docs/ROADMAP.md`.

Related-project research and positioning notes live in `docs/PRIOR_ART.md` and
`docs/POSITIONING.md`.

Current v0 hardware target: **Waveshare ESP32-S3-LCD-1.54 + battery**. It keeps
the important recorder pieces under $30: dual mic + ES7210 ADC, battery
charging, TF/microSD, Wi-Fi, and a small status LCD.

## Quick start (server)

This project is **uv-managed**. Install [uv](https://docs.astral.sh/uv/), then:

```bash
cd server
uv sync                                        # create .venv + install from uv.lock
cp .env.example .env                           # add your transcription + LLM API keys
uv run uvicorn open_plaud.api.app:app --reload
```

Then open **http://127.0.0.1:8000/** for the web UI (list, play, transcript,
summary, Markdown export), or use the device upload target directly:

```bash
curl -F "file=@rec_0001.wav;type=audio/wav" http://127.0.0.1:8000/v1/recordings
# -> {"id":"...","status":"received","filename":"rec_0001.wav","size_bytes":...}
# the pipeline runs in the background; poll GET /v1/recordings/{id} for the result
```

By default the pipeline is a no-op (stores audio, empty transcript) so it runs
with no API keys. To actually transcribe + summarize, set the keys in `.env` and
select the cloud providers:

```bash
OP_PIPELINE__TRANSCRIPTION=openai_whisper   # needs OPENAI_API_KEY
OP_PIPELINE__SUMMARIZATION=claude           # needs ANTHROPIC_API_KEY
```

`uv` reads the pinned interpreter from `.python-version` (installing it if missing).
Dev tools (pytest, ruff, mypy) come with `uv sync` — run them via `uv run pytest`,
`uv run ruff check .`, `uv run mypy .`.

## License

MIT. See `LICENSE`.
