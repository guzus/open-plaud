"""OpenAI Whisper transcription provider.

Calls the OpenAI `audio/transcriptions` endpoint. Registered under the name
`openai_whisper`; the registry instantiates it with no args, so every
constructor parameter has a default. The API key is read lazily in `__init__`
(not at import time) so merely importing the package — which is what registers
the provider — never requires a key.
"""
from __future__ import annotations

import asyncio
import os

import httpx

from open_plaud.transcription.base import register
from open_plaud.types import AudioRef, TranscribeOptions, Transcript, TranscriptSegment

_API_URL = "https://api.openai.com/v1/audio/transcriptions"
_DEFAULT_MODEL = "whisper-1"
_REQUEST_TIMEOUT = 120.0


@register
class OpenAiWhisperTranscriber:
    name = "openai_whisper"

    def __init__(
        self,
        api_key: str | None = None,
        model: str | None = None,
        http_client: httpx.AsyncClient | None = None,
    ) -> None:
        # Resolve the key now and fail fast: the pipeline wires providers once at
        # startup, so a missing key should blow up loudly there, not mid-request.
        api_key = api_key or os.environ.get("OPENAI_API_KEY")
        if not api_key:
            raise RuntimeError(
                "OPENAI_API_KEY is required for the openai_whisper transcription provider"
            )
        self._api_key = api_key
        self._model = model or os.environ.get("OP_WHISPER_MODEL", _DEFAULT_MODEL)
        self._client = http_client

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        # Read off the event loop: file I/O is blocking.
        content = await asyncio.to_thread(audio.path.read_bytes)

        content_type = audio.content_type or "application/octet-stream"
        files: dict[str, tuple[str, bytes, str]] = {
            "file": (audio.filename, content, content_type),
        }
        data: dict[str, str] = {"model": self._model, "response_format": "verbose_json"}
        if opts.language:
            data["language"] = opts.language
        headers = {"Authorization": f"Bearer {self._api_key}"}

        if self._client is not None:
            response = await self._client.post(
                _API_URL, headers=headers, files=files, data=data
            )
        else:
            async with httpx.AsyncClient(timeout=_REQUEST_TIMEOUT) as client:
                response = await client.post(
                    _API_URL, headers=headers, files=files, data=data
                )

        if not response.is_success:
            raise RuntimeError(
                f"openai_whisper transcription failed "
                f"(HTTP {response.status_code}): {response.text}"
            )

        payload = response.json()
        if "text" not in payload:
            raise RuntimeError(
                f"openai_whisper: unexpected response shape (no 'text'): {payload!r}"
            )

        segments: list[TranscriptSegment] = []
        raw_segments = payload.get("segments", [])
        if isinstance(raw_segments, list):
            for item in raw_segments:
                if not isinstance(item, dict) or not isinstance(item.get("text"), str):
                    continue
                raw_start = item.get("start")
                raw_end = item.get("end")
                segments.append(
                    TranscriptSegment(
                        text=item["text"].strip(),
                        start=(
                            float(raw_start)
                            if isinstance(raw_start, int | float)
                            else None
                        ),
                        end=(
                            float(raw_end)
                            if isinstance(raw_end, int | float)
                            else None
                        ),
                    )
                )
        return Transcript(text=payload["text"], language=opts.language, segments=segments)
