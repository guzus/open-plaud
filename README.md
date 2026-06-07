# open-plaud

An open-source, self-hostable alternative to AI voice recorders like Plaud — DIY hardware, a pluggable transcription pipeline, and AI summarization you control.

No per-minute limits. No forced cloud lock-in. No subscription. You own the recordings and the pipeline.

## Why

Commercial AI recorders ship cheap hardware and monetize a cloud subscription that caps your transcription minutes and processes your private audio on someone else's servers. `open-plaud` inverts that: the value lives in an open pipeline you run yourself.

| | Plaud (commercial) | open-plaud |
|---|---|---|
| Hardware | Closed, ~$159+ | Open, ~₩30–40k DIY |
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

Early scaffold. The transcription layer is **cloud-first today** with a provider-agnostic interface, so on-device (desktop/mobile) inference can be added later without touching the rest of the pipeline. See `docs/ARCHITECTURE.md` and `docs/ROADMAP.md`.

## Quick start (server)

```bash
cd server
python -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
cp .env.example .env   # add your transcription + LLM API keys
uvicorn open_plaud.api.app:app --reload
```

## License

MIT. See `LICENSE`.
