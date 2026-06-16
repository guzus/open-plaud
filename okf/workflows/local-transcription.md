---
type: Workflow
title: Local Transcription
description: Configure the server to transcribe locally with whisper.cpp and optionally summarize locally with Ollama.
resource: ../../README.md
tags: [workflow, local, transcription, ollama]
timestamp: 2026-06-16T00:00:00+09:00
---
# When To Use

Use this workflow when private audio should stay on local hardware after device
upload, or when avoiding per-minute cloud transcription cost matters more than
using a managed transcription API.

# Transcription Settings

```bash
OP_PIPELINE__TRANSCRIPTION=whisper_cpp
OP_WHISPER_CPP_MODEL=/path/to/ggml-model.bin
# OP_WHISPER_CPP_BIN=whisper-cli
```

The `whisper_cpp` provider shells out to a local `whisper-cli`, asks it for JSON
output, parses segment timing, and returns the standard transcript model.

# Optional Local Summarization

```bash
OP_PIPELINE__SUMMARIZATION=ollama
OP_OLLAMA_MODEL=qwen2.5:7b
# OP_OLLAMA_URL=http://127.0.0.1:11434
```

The Ollama provider requests JSON with `summary` and `action_items`, then
validates that shape before returning a `Summary`.

# Related Concepts

* [Server](/components/server.md)
* [Pipeline provider contract](/contracts/pipeline-provider-contract.md)
* [Recording data model](/contracts/recording-data-model.md)

# Citations

[1] [README quick start](../../README.md)
[2] [whisper.cpp provider](../../server/open_plaud/transcription/whisper_cpp.py)
[3] [Ollama summarizer](../../server/open_plaud/summarization/ollama.py)
