# Vibecoding Device Mode

`open-plaud` can act as a Plaud-like voice input device for a Hooker coding-agent
loop. The device still stays dumb: it records WAV, uploads it, and lets the
server transcribe. The optional Hooker dispatcher turns the completed transcript
into a Hooker topic message.

## Flow

```text
hold button -> record voice command -> POST /v1/recordings
  -> transcribe/summarize in open-plaud
  -> POST {HOOKER_URL}/publish/{HOOKER_TOPIC}
  -> hooker-agent-bridge consumes the input topic
  -> OpenCode or Pi runs the coding task
  -> Hooker publishes the response to the output topic/routes
```

This is best for scoped spoken commands such as:

- "Inspect the failing tests and propose a fix."
- "Create a plan for adding GitHub OAuth."
- "Status."
- "Stop."

It is not low-latency live dictation. Treat v1 as a press-to-dispatch prompt
device.

## Configuration

Hooker dispatch is disabled by default.

```bash
OP_PIPELINE__TRANSCRIPTION=openai_whisper   # or whisper_cpp
OP_PIPELINE__SUMMARIZATION=claude           # optional, or ollama

OP_HOOKER__ENABLED=true
OP_HOOKER__URL=https://your-hooker.example
OP_HOOKER__API_KEY=hk_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
OP_HOOKER__TOPIC=agent_in
```

Optional settings:

```bash
OP_HOOKER__PRIORITY=3
OP_HOOKER__TAGS=agent,user,voice,open-plaud
OP_HOOKER__TIMEOUT_SECONDS=15
OP_HOOKER__PROMPT_FOOTER=Treat this as a spoken vibecoding command. If the request is ambiguous or could mutate files in a risky way, ask for clarification or propose a plan before changing code.
```

Use a scoped Hooker token that can publish only to the input topic if possible.
The dispatcher never needs Hooker admin access.

## Published Message Contract

For recording `rec_0001.wav` on topic `agent_in`, the dispatcher sends:

```http
POST /publish/agent_in
Authorization: Bearer hk_...
Content-Type: application/json
Idempotency-Key: open-plaud:recording:{recording_id}:hooker:v1
```

Body:

```json
{
  "title": "Voice prompt: rec_0001.wav",
  "text": "Voice prompt from open-plaud.\n\nRecording id: ...\nFilename: rec_0001.wav\n\nTranscript:\n...\n\nTreat this as a spoken vibecoding command...",
  "priority": 3,
  "tags": ["agent", "user", "voice", "open-plaud"]
}
```

The `text` field is what `hooker-agent-bridge` forwards to the coding harness,
so keep the prompt footer short and operational.

## Failure Behavior

- If dispatch is disabled, no Hooker request is made.
- If transcription returns an empty transcript, dispatch is skipped.
- If Hooker returns a non-2xx response or is unavailable, the recording remains
  usable and keeps status `done`; `error` is set to
  `Hooker dispatch failed: ...`.
- Reprocessing a recording from the web UI reruns the pipeline and retries
  Hooker dispatch with the same idempotency key.

For production use, pair this with Hooker's idempotency and topic history. If
you need guaranteed dispatch after process restarts, add a durable outbox worker
instead of relying only on the FastAPI background task.
