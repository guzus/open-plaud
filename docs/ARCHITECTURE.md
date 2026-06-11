# Architecture

## Design goals

1. **Cloud-first, local-ready.** Transcription works out of the box with a cloud API, but every transcription provider implements the same interface. Adding desktop/mobile local inference later is a new class, not a rewrite.
2. **Pipeline is provider-agnostic end to end.** Transcription, diarization, and summarization are each swappable behind a protocol. The orchestrator never imports a concrete provider.
3. **The device is dumb on purpose.** Firmware records WAV and uploads. All intelligence lives server-side, so the same firmware works as models improve.

## Data flow

```
[ESP32-S3 device]
   record WAV  --->  microSD
   Wi-Fi sync  --->  POST /v1/recordings (multipart audio)
                          |
                          v
                  [ingest + storage]      raw audio persisted, Recording row created
                          |
                          v
                  [pipeline orchestrator]  async job, status tracked
                          |
        +-----------------+------------------+
        v                 v                  v
  [transcription]    [diarization]     [summarization]
  Provider (cloud)   speaker turns     LLM (Claude/GPT/...)
        |                 |                  |
        +-----------------+------------------+
                          v
                  [Transcript + Summary persisted]
                          |
                          v
                  GET /v1/recordings/{id}   client fetches text, speakers, summary
```

## The pluggable layer (why this matters)

Each stage is defined by a `Protocol`. Concrete providers register themselves; the orchestrator resolves them by name from config. This is what lets "cloud now → local later" happen cleanly.

```python
class TranscriptionProvider(Protocol):
    name: str
    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript: ...
```

Providers planned:

| Stage | Implemented | Later |
|---|---|---|
| Transcription | OpenAI Whisper API (cloud), `whisper_cpp` (local, wraps `whisper-cli`) | Deepgram, Groq-Whisper, on-device mobile (Core ML / TFLite) |
| Diarization | pyannote via API, AssemblyAI | local `pyannote.audio`, `whisperx` |
| Summarization | Claude, GPT, Gemini | local Llama/Qwen via Ollama |

Selection is config-driven:

```toml
[pipeline]
transcription = "openai_whisper"
diarization   = "none"          # opt-in; off by default
summarization = "claude"
```

Switching to local is just `transcription = "whisper_cpp"` — that provider wraps
the whisper.cpp CLI (`OP_WHISPER_CPP_BIN` / `OP_WHISPER_CPP_MODEL`); no
orchestrator change was needed, which is the point of the interface.

## Why cloud-first (per project decision)

Local inference (whisper.cpp / on-device) is on the roadmap, not the v0 default. Cloud-first gets a working end-to-end loop fastest and keeps the device firmware identical regardless of where transcription runs. The interface is designed so the local backends slot in as additional providers.

Current repository state: the loop runs end to end. The `openai_whisper` (cloud) and `whisper_cpp` (local) transcription providers and the `claude` summarization provider are implemented behind the protocols; the upload endpoint runs the pipeline as a background task; recordings persist in SQLite. The first "local later" backend has landed exactly as designed — as a new provider, with no orchestrator change. On-device mobile remains future work behind the same interface.

## Components

- `server/open_plaud/api` — FastAPI app, routes, schemas, + Jinja2 web UI (`templates/`)
- `server/open_plaud/pipeline` — orchestrator + job state machine
- `server/open_plaud/transcription` — provider protocol + `none` / `openai_whisper` / `whisper_cpp`
- `server/open_plaud/diarization` — provider protocol (no-op default)
- `server/open_plaud/summarization` — LLM provider protocol + `none` / `claude`
- `server/open_plaud/store.py` — SQLite persistence; audio blobs on local FS
- `firmware/` — ESP-IDF project for the recorder
- `hardware/` — BOM, case, wiring
