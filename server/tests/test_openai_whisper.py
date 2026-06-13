from __future__ import annotations

from pathlib import Path

import httpx
import pytest

from open_plaud.transcription.openai_whisper import OpenAiWhisperTranscriber
from open_plaud.types import AudioRef, TranscribeOptions


def _audio(tmp_path: Path, content: bytes = b"RIFF....WAVEfmt ") -> AudioRef:
    path = tmp_path / "rec.wav"
    path.write_bytes(content)
    return AudioRef(
        path=path,
        filename="rec.wav",
        content_type="audio/wav",
        size_bytes=len(content),
    )


async def test_transcribe_returns_transcript_and_sends_audio(tmp_path: Path) -> None:
    audio_bytes = b"RIFF1234WAVEfmt audio-body"
    captured: dict[str, bytes] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        # Multipart bodies stream, so `request.content` would raise; read() works.
        body = request.read()
        captured["body"] = body
        captured["auth"] = request.headers["Authorization"].encode()
        return httpx.Response(
            200,
            json={
                "text": "hello world",
                "segments": [{"start": 0.0, "end": 1.5, "text": "hello world"}],
            },
        )

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    try:
        provider = OpenAiWhisperTranscriber(
            api_key="test-key", model="whisper-1", http_client=client
        )
        result = await provider.transcribe(
            _audio(tmp_path, audio_bytes), TranscribeOptions()
        )
    finally:
        await client.aclose()

    assert result.text == "hello world"
    assert result.language is None
    assert len(result.segments) == 1
    assert result.segments[0].text == "hello world"
    assert result.segments[0].start == 0.0
    assert result.segments[0].end == 1.5
    body = captured["body"]
    assert audio_bytes in body  # the file bytes were uploaded
    assert b'name="file"' in body
    assert b"whisper-1" in body  # the model field is present
    assert b"verbose_json" in body
    assert captured["auth"] == b"Bearer test-key"


async def test_missing_api_key_raises(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
    with pytest.raises(RuntimeError, match="OPENAI_API_KEY is required"):
        OpenAiWhisperTranscriber()


async def test_non_2xx_response_raises(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(401, text="unauthorized")

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    try:
        provider = OpenAiWhisperTranscriber(
            api_key="test-key", model="whisper-1", http_client=client
        )
        with pytest.raises(RuntimeError, match="401"):
            await provider.transcribe(_audio(tmp_path), TranscribeOptions())
    finally:
        await client.aclose()


async def test_language_is_forwarded_when_set(tmp_path: Path) -> None:
    captured: dict[str, bytes] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        captured["body"] = request.read()
        return httpx.Response(200, json={"text": "bonjour"})

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    try:
        provider = OpenAiWhisperTranscriber(
            api_key="test-key", model="whisper-1", http_client=client
        )
        result = await provider.transcribe(
            _audio(tmp_path), TranscribeOptions(language="fr")
        )
    finally:
        await client.aclose()

    assert result.language == "fr"
    body = captured["body"]
    assert b'name="language"' in body
    assert b"fr" in body
