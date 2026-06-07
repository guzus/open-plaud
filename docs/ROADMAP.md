# Roadmap

## v0 — working loop (current scaffold)
- [x] Provider-agnostic pipeline (transcription / diarization / summarization)
- [x] Cloud-first transcription (OpenAI Whisper API)
- [x] Claude summarization with structured action items
- [x] FastAPI ingest endpoint (device upload target)
- [x] In-memory store + local-FS audio blobs
- [ ] SQLite persistence (replace in-memory store)
- [ ] Minimal web UI to browse/play/export recordings

## v1 — hardware reference
- [ ] ESP-IDF firmware for ESP32-S3-AUDIO-Board (dual-mic, ES7210 ADC)
- [ ] Record WAV to microSD, hold-to-record + tag
- [ ] Wi-Fi sync to `POST /v1/recordings`
- [ ] 3D-printable card-style case (MagSafe magnet optional)

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
