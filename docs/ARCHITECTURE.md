# Architecture

## Design goals

1. **Cloud-first, local-capable.** Transcription works with a cloud API or local providers, and every transcription provider implements the same interface. Adding more desktop/mobile inference later is a new class, not a rewrite.
2. **Pipeline is provider-agnostic end to end.** Transcription, diarization, and summarization are each swappable behind a protocol. The orchestrator never imports a concrete provider.
3. **The device is dumb on purpose.** Firmware records WAV and uploads. All intelligence lives server-side, so the same firmware works as models improve. This is not a stopgap — see [Why no on-device LLM](#why-no-on-device-llm-and-what-does-fit).

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

## Capture, storage, and transfer formats

These are three different things, and conflating them causes bad "optimizations".

| Stage | Format | Rate | Why |
|---|---|---|---|
| **Capture** (I2S) | 16-bit **stereo** | 230 MB/h | The ES7210 exposes two mics as two TDM slots; the PDM path uses stereo slot mode. This is the board-native wire format, not a storage choice. |
| **Storage** (microSD) | 16-bit **mono** PCM WAV | **115 MB/h** | `app_main.c` averages L+R (`pcm_to_mono()`) before every `wav_write()`; `WAV_CHANNELS` is already `1`. |
| **Transfer** (Wi-Fi upload) | Ogg Opus, else Open Plaud ADPCM, else mono PCM | ~10 / 28.8 / 115 MB/h | `net_upload.c`, gated by `OPEN_PLAUD_COMPRESS_TRANSFERS` / `OPEN_PLAUD_OPUS_TRANSFERS`. |

Two consequences worth stating explicitly, because both get proposed as bugs:

- **`AUDIO_CHANNELS` is 2 and should stay 2.** It describes what the codec
  clocks out, not what lands on the card. Setting it to 1 would not save a
  single SD byte — the downmix already happened — and it would break the ES7210
  two-slot TDM configuration while throwing away the ~3 dB of noise averaging
  the second mic buys.
- **The server must accept more than PCM WAV.** With the shipped firmware
  defaults, a clip arrives as `rec_NNNN.opus` (`audio/ogg`) or, with Opus off,
  as the `OPADPCM1` container (`application/vnd.openplaud.adpcm`). Ingest
  decodes the ADPCM container to canonical PCM WAV on arrival so that every
  downstream provider — including `whisper_cpp`, which reads only 16-bit PCM —
  keeps working unchanged.

### Why the card still holds uncompressed PCM

Compressing the *stored* clip (rather than only the transferred one) would cut
SD writes ~4x, and it is a reasonable thing to want. It is deliberately not done
yet, for two reasons:

- **PCM on the card is the archival format everything else assumes.** The BLE
  chunker (`ble_companion.c`) seeks into the WAV by sample offset, and
  `make_upload_plan()` in `net_upload.c` refuses anything that is not canonical
  16-bit PCM. Both would need to change together, and a partially-written
  compressed clip after a crash is far harder to salvage than a truncated PCM one.
- **SD bytes are not the binding constraint.** At 115 MB/h even a 32 GB card
  holds ~278 h of audio, against a battery that lasts an estimated 2–5 h. The
  card outlasts the cell by roughly two orders of magnitude. The write *energy*
  might still be worth reclaiming — but nobody has measured it, so there is no
  evidence it pays for the complexity. See `docs/POWER.md`.

Sequence matters here: measure the four steady-state currents first, then decide
whether on-card compression (or Opus, which costs meaningfully more CPU) earns
its keep.

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

## Why no on-device LLM (and what *does* fit)

This is the most frequently proposed change to the project, so here is the
arithmetic once, with primary sources. **It does not fit, and it is not close.**

The recorder is an ESP32-S3R8: **8 MB of octal PSRAM** and 16 MB of flash
(`firmware/sdkconfig.defaults`). That 8 MB is the entire budget a model would
have to live in — shared with the audio DMA buffers, the FATFS/SD stack, the
LCD framebuffer, and the Wi-Fi/TLS stack.

| Candidate | Published size | vs. 8 MB PSRAM |
|---|---|---|
| Qwen2.5-0.5B-Instruct, Q4_K_M GGUF | **491 MB** | **~61x over** |
| Qwen2.5-0.5B-Instruct, Q2_K GGUF | **415 MB** | **~52x over** |
| Whisper `tiny`, memory required to run | **~273 MB** | **~34x over** |

Three things follow, and each one kills the idea independently:

1. **Quantizing harder does not rescue it.** Going from Q4_K_M (491 MB) to Q2_K
   (415 MB) — a far more destructive quantization — buys only **15%**. At the
   0.5B scale the token-embedding matrix and the output head dominate the file,
   and those are kept at higher precision precisely because crushing them
   destroys the model. You cannot quantize your way across a 60x gap.
2. **Whisper needs ~273 MB just to run**, and `whisper.cpp` has no
   microcontroller target at all — it targets hosts with an OS, a real
   allocator, and SIMD. `tiny` is 75 MiB on disk but ~273 MB resident.
3. **Better silicon does not close it either.** The largest ESP32-P4 variant
   ships 32 MB of PSRAM. 491 MB against 32 MB is still **~15x over**. This is a
   category gap, not a next-generation-part gap.

For comparison, the devices this project is measured against made the same call:
Omi's nRF5340 wearable runs **no** ASR, **no** wake word, and **no** LLM on the
device, and Plaud Note Pro does all transcription in the cloud.

### What *is* achievable on-device

Espressif's ESP-SR does run real speech models on this exact SoC — they are just
not transcription or generation:

| Feature | Cost (ESP32-S3) | What you actually get |
|---|---|---|
| **WakeNet9** (wake word), quantized, 2-channel | **16 KB internal RAM + 324 KB PSRAM**, 3.0 ms per 32 ms frame (**~9% of one core**) | Detects *one* fixed wake phrase. Not transcription. |
| **MultiNet7** (command words), quantized | **18 KB internal RAM + 2.92 MB PSRAM** | At most **200 FIXED commands**, **Chinese/English only** |

MultiNet's limits are the important part: the command list is fixed at build
time, and the docs state plainly that **mixed Chinese and English is not
supported in command words**. There is no Korean model, so for this project's
Korean-language use case there is **no on-device recognition path at all** —
only cloud or desktop transcription.

And note what 2.92 MB of PSRAM for 200 fixed commands implies about the budget:
MultiNet alone would consume ~37% of the SoC's entire PSRAM to recognize a
closed vocabulary. That is the same budget the device needs for capture buffers
and the endurance work below.

### The real gap is endurance, not intelligence

Against Plaud (30 h recording, 60-day standby, 64 GB), open-plaud's estimated
**2–5 h of continuous recording** (`docs/POWER.md`) is where the product loses.
On-device inference would spend exactly the power budget needed to close that
gap, to do worse than the server already does for free. Every mW is better spent
on capture efficiency and standby draw.

**Sources**
- Qwen2.5-0.5B-Instruct GGUF file sizes — <https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF>
- whisper.cpp model disk/memory table — <https://github.com/ggml-org/whisper.cpp>
- ESP-SR WakeNet/MultiNet resource benchmarks — <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html>
- MultiNet language/command limits — <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html>

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
