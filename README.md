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

Early scaffold. The pipeline layer is provider-agnostic, and concrete cloud transcription/summarization providers are the next server milestone. On-device (desktop/mobile) inference can be added later without touching the rest of the pipeline. See `docs/ARCHITECTURE.md` and `docs/ROADMAP.md`.

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

Device upload target:

```bash
curl -F "file=@rec_0001.wav;type=audio/wav" http://127.0.0.1:8000/v1/recordings
# -> {"id":"...","status":"transcribing","filename":"rec_0001.wav","size_bytes":...}
```

`uv` reads the pinned interpreter from `.python-version` (installing it if missing).
Dev tools (pytest, ruff, mypy) come with `uv sync` — run them via `uv run pytest`,
`uv run ruff check .`, `uv run mypy .`.

## License

MIT. See `LICENSE`.
