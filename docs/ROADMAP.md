# Roadmap

## v0 — working loop (current scaffold)
- [x] Provider-agnostic pipeline interfaces (transcription / diarization / summarization)
- [x] Cloud-first transcription provider (OpenAI Whisper API) — `openai_whisper`
- [x] Claude summarization with structured action items (tool-forced `{summary, action_items[]}`)
- [x] FastAPI ingest endpoint runs the pipeline as a background task on upload
- [x] Local-FS audio blobs
- [x] SQLite persistence (replaces the in-memory store; survives restart)
- [x] Minimal web UI to browse/play/export recordings (`GET /`, `/rec/{id}`)

## v1 — hardware reference
- [x] Select v0 hardware: Waveshare ESP32-S3-LCD-1.54 + battery (dual mic, ES7210, TF, LCD)
- [~] ESP-IDF firmware for ESP32-S3-LCD-1.54 — builds clean on ESP-IDF v5.4.1
      (`esp_codec_dev` 2.0.0); flash + runtime validation pending board arrival
- [~] Recorder loop implemented: hold-to-record (PLUS), numbered `rec_NNNN.wav`
      clips, `/sdcard/sent` + next-boot upload retry queue — hardware-unvalidated
- [~] Wi-Fi sync to `POST /v1/recordings` (implemented, not hardware-flashed yet)
- [~] 3D-printable case for LCD-1.54 + battery (first-article STL exists; measure after board arrives)

## v2 — local transcription (the deferred decision)
The interface was already built for this. These are new providers, not rewrites.
- [x] `whisper_cpp` desktop provider — subprocess wrapper around `whisper-cli`; no cloud
      key. Smoke-tested against real whisper.cpp (brew 1.8.x) + `ggml-base.en`: mono and
      device-shaped stereo 16 kHz WAVs, end-to-end through `POST /v1/recordings`
- [x] `OP_PIPELINE__TRANSCRIPTION=whisper_cpp` switch documented (`server/.env.example`, README)
- [ ] On-device mobile transcription (Core ML / TFLite)

## v3 — speaker intelligence
- [ ] `pyannote` / `whisperx` diarization providers
- [ ] Speaker labels merged into transcript segments
- [ ] Local LLM summarization via Ollama

## Design invariant
Every new capability arrives as a provider behind an existing Protocol. If a
feature requires changing the orchestrator, reconsider the interface first.
