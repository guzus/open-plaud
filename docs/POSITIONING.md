# Positioning

Last updated: 2026-06-08.

## Category

`open-plaud` is an open reference stack for a Plaud-like recorder:

1. Reference hardware: documented BOM and 3D-printable enclosure.
2. Device firmware: ESP32-S3 records WAV to microSD and syncs to a server.
3. Self-hosted pipeline: FastAPI ingest, local storage, pluggable transcription, diarization, and summarization providers.

This is narrower than "AI wearable" and broader than "Whisper web UI."

## Positioning Statement

For people who want a dedicated voice recorder without subscription lock-in, `open-plaud` provides a DIY ESP32-S3 recorder and self-hosted AI pipeline. Unlike Riffado, it does not require a Plaud device or Plaud account. Unlike self-hosted transcription apps, it includes the capture hardware and firmware path.

## Claims To Make

- Open reference recorder stack: hardware notes, firmware, and server live together.
- Dumb device, smart server: the recorder captures audio reliably; models and prompts evolve server-side.
- Self-hostable custody: recordings and derived text can live in storage the operator controls.
- Provider-agnostic pipeline: cloud providers work now; local providers can fit behind the same interfaces.
- DIY-first: the point is inspectability and hackability, not matching commercial thinness on v0.

## Claims To Avoid

- "First open-source Plaud alternative." Riffado/OpenPlaud and Applaud already occupy parts of that language.
- "OpenPlaud" as a brand. That name redirects to Riffado publicly.
- "Fully local" until local transcription and summarization providers are implemented and documented.
- "Privacy-preserving" without qualifiers. Cloud transcription or LLM providers still receive audio/text when selected.
- "Production hardware" before real board flashing, audio validation, enclosure fit, battery behavior, and upload reliability are proven.

## Comparison Short Form

| Alternative | User already has | `open-plaud` difference |
|---|---|---|
| Plaud | Polished closed recorder and cloud AI service. | Open DIY recorder path and self-hosted pipeline control. |
| Riffado | Plaud device/account and a desire to self-host the companion app. | No Plaud hardware or account dependency; build the recorder yourself. |
| Applaud, Speakr, Scriberr, Transcription Stream | Audio files and a server or desktop machine. | Adds the physical recorder and firmware path that creates those audio files. |
| Omi | Wearable memory platform with mobile app and broad integrations. | Narrow recorder appliance: local WAV capture, upload, transcription, summaries. |
| Voice Terminal / voice assistants | Interactive voice control and TTS loops. | Asynchronous recording and note pipeline, not assistant interaction. |

## Audience

Primary:

- Makers who want a dedicated recorder they can inspect, flash, and repair.
- Self-hosters who want ownership of recordings and transcripts.
- Developers who want a small hardware reference for audio capture feeding an AI pipeline.

Secondary:

- Journalists, researchers, consultants, and clinicians evaluating custody trade-offs. These users need explicit caveats around consent, local/cloud provider choice, and compliance.

Not primary:

- Users who want a polished consumer app with no setup.
- Users who only need desktop meeting capture.
- Users who already own Plaud hardware and only need self-hosted sync.

## README Language

Preferred:

> An open reference stack for a Plaud-like recorder: DIY ESP32-S3 hardware, firmware that records and syncs WAV files, and a self-hosted transcription/summarization server.

Acceptable:

> A self-hostable alternative to closed AI voice-recorder ecosystems.

Avoid:

> OpenPlaud.

> The open-source Plaud replacement.

> Fully private AI transcription.

## Roadmap Implications

The most valuable next work is not another transcript UI. Existing projects are already strong there. The positioning gets stronger from:

- Flash-and-verify instructions for the selected Waveshare ESP32-S3-LCD-1.54 board.
- Known-good audio samples and recording-quality notes.
- Repeatable case/BOM assembly guide.
- Upload reliability tests from firmware to `POST /v1/recordings`.
- A local transcription provider such as `whisper.cpp` or another OpenAI-compatible local speech endpoint.
- A clear trademark/non-affiliation note before broad public promotion.
