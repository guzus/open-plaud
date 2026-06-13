# Architecture

## Design goals

1. **Cloud-first, local-capable.** Transcription works with a cloud API or local providers, and every transcription provider implements the same interface. Adding more desktop/mobile inference later is a new class, not a rewrite.
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
  Provider           speaker turns     LLM
        |                 |                  |
        +-----------------+------------------+
                          v
                  [Transcript + Summary persisted]
                          |
                          v
                  GET /v1/recordings/{id}   client fetches text, speakers, summary
```

## The pluggable layer (why this matters)

Each stage is defined by a `Protocol`. Concrete providers register themselves; the orchestrator resolves them by name from config. This is what lets cloud and local providers coexist without changing the pipeline.

```python
class TranscriptionProvider(Protocol):
    name: str
    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript: ...
```

Providers:

| Stage | Implemented | Later |
|---|---|---|
| Transcription | `none`, OpenAI Whisper API, `whisper_cpp` CLI, iOS on-device Speech package | Core ML / TFLite Whisper engine |
| Diarization | `none`, `pyannote`, `whisperx` | AssemblyAI or other hosted diarization |
| Summarization | `none`, Claude, Ollama | GPT, Gemini, other local models |

Selection is config-driven:

```toml
[pipeline]
transcription = "openai_whisper" # or "whisper_cpp"
diarization   = "none"          # opt-in: "pyannote" or "whisperx"
summarization = "claude"         # or "ollama"
```

Switching to local desktop transcription is just `transcription = "whisper_cpp"`
plus `OP_WHISPER_CPP_MODEL=/path/to/model.bin` — no orchestrator change.

## Why providers, not forks

Cloud providers still get a working end-to-end loop fastest for many users, while local providers keep private audio on hardware you control. The device firmware stays identical either way; choosing OpenAI Whisper, `whisper_cpp`, iOS Speech, Claude, Ollama, or diarization is a server/mobile configuration decision, not a firmware fork.

Current repository state: the loop runs end to end. The `openai_whisper` and
`whisper_cpp` server transcription providers, the iOS on-device Speech package,
the `pyannote` / `whisperx` diarization providers, plus the `claude` and
`ollama` summarization providers are implemented behind small contracts; the
upload endpoint runs the server pipeline as a background task; recordings
persist in SQLite. A bundled Core ML / TFLite Whisper mobile engine remains a
future optional backend.

## Components

- `server/open_plaud/api` — FastAPI app, routes, schemas, + Jinja2 web UI (`templates/`)
- `server/open_plaud/pipeline` — orchestrator + job state machine
- `server/open_plaud/transcription` — provider protocol + `none` / `openai_whisper` / `whisper_cpp`
- `mobile/ios` — SwiftPM package for iOS on-device transcription
- `server/open_plaud/diarization` — provider protocol + `none` / `pyannote` / `whisperx`
- `server/open_plaud/summarization` — LLM provider protocol + `none` / `claude` / `ollama`
- `server/open_plaud/store.py` — SQLite persistence; audio blobs on local FS
- `firmware/` — ESP-IDF project for the recorder
- `hardware/` — BOM, case, wiring
