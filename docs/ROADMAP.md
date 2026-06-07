# Roadmap

## v0 — working loop (current scaffold)
- [x] Provider-agnostic pipeline interfaces (transcription / diarization / summarization)
- [ ] Cloud-first transcription provider (OpenAI Whisper API)
- [ ] Claude summarization with structured action items
- [x] FastAPI ingest endpoint (device upload target)
- [x] In-memory store + local-FS audio blobs
- [ ] SQLite persistence (replace in-memory store)
- [ ] Minimal web UI to browse/play/export recordings

## v1 — hardware reference
- [~] ESP-IDF firmware scaffold for ESP32-S3-AUDIO-Board (dual-mic, ES7210 ADC) — `firmware/`
- [~] Record WAV to microSD (bring-up path done); hold-to-record + tag still TODO
- [~] Wi-Fi sync to `POST /v1/recordings` (implemented, not hardware-flashed yet)
- [~] 3D-printable case — parametric STL generator in `hardware/case/` (first-article)

## v2 — local transcription (the deferred decision)
The interface is already built for this. These are new providers, not rewrites.
- [ ] `whisper_cpp` desktop provider (registered stub exists today)
- [ ] On-device mobile transcription (Core ML / TFLite)
- [ ] `OP_TRANSCRIPTION=whisper_cpp` switch documented

## v3 — speaker intelligence
- [ ] `pyannote` / `whisperx` diarization providers
- [ ] Speaker labels merged into transcript segments
- [ ] Local LLM summarization via Ollama

## Design invariant
Every new capability arrives as a provider behind an existing Protocol. If a
feature requires changing the orchestrator, reconsider the interface first.
