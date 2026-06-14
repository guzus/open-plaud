from __future__ import annotations

import json
from pathlib import Path

import httpx
import pytest

from open_plaud.config import Config, HookerConfig
from open_plaud.hooker import HookerDispatchError, HookerDispatcher
from open_plaud.types import ActionItem, AudioRef, Recording, Summary, Transcript


def _recording(transcript: str = "inspect failing tests and propose a fix") -> Recording:
    return Recording(
        id="rec-123",
        audio=AudioRef(
            path=Path("/tmp/rec.wav"),
            filename="rec.wav",
            content_type="audio/wav",
            size_bytes=128,
        ),
        transcript=Transcript(text=transcript),
        summary=Summary(
            summary="The user asked for a coding-agent task.",
            action_items=[ActionItem(text="inspect tests", owner=None, due=None)],
        ),
    )


async def test_dispatch_skips_when_disabled() -> None:
    async with httpx.AsyncClient(transport=httpx.MockTransport(_unexpected_request)) as client:
        dispatcher = HookerDispatcher(HookerConfig(enabled=False), http_client=client)
        result = await dispatcher.dispatch(_recording())

    assert result.published is False
    assert result.reason == "disabled"


async def test_dispatch_skips_empty_transcript() -> None:
    async with httpx.AsyncClient(transport=httpx.MockTransport(_unexpected_request)) as client:
        dispatcher = HookerDispatcher(_enabled_config(), http_client=client)
        result = await dispatcher.dispatch(_recording(transcript="  "))

    assert result.published is False
    assert result.reason == "empty_transcript"


async def test_dispatch_posts_vibecoding_prompt_to_hooker() -> None:
    captured: dict[str, object] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        captured["url"] = str(request.url)
        captured["authorization"] = request.headers["authorization"]
        captured["idempotency_key"] = request.headers["idempotency-key"]
        captured["body"] = json.loads(request.content)
        return httpx.Response(200, json={"ok": True})

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        dispatcher = HookerDispatcher(_enabled_config(), http_client=client)
        result = await dispatcher.dispatch(_recording())

    assert result.published is True
    assert captured["url"] == "https://hooker.test/publish/agent_in"
    assert captured["authorization"] == "Bearer hk_test"
    assert captured["idempotency_key"] == "open-plaud:recording:rec-123:hooker:v1"
    body = captured["body"]
    assert isinstance(body, dict)
    assert body["title"] == "Voice prompt: rec.wav"
    assert body["priority"] == 3
    assert body["tags"] == ["agent", "user", "voice", "open-plaud"]
    assert "inspect failing tests and propose a fix" in body["text"]
    assert "Treat this as a spoken vibecoding command" in body["text"]


async def test_dispatch_raises_for_non_2xx_response() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(401, text="bad token")

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        dispatcher = HookerDispatcher(_enabled_config(), http_client=client)
        with pytest.raises(HookerDispatchError, match="401"):
            await dispatcher.dispatch(_recording())


async def test_dispatch_requires_url_and_token_when_enabled() -> None:
    dispatcher = HookerDispatcher(HookerConfig(enabled=True, url="", api_key=""))

    with pytest.raises(HookerDispatchError, match="OP_HOOKER__URL"):
        await dispatcher.dispatch(_recording())


def test_config_reads_nested_hooker_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("OP_HOOKER__ENABLED", "true")
    monkeypatch.setenv("OP_HOOKER__URL", "https://hooker.example")
    monkeypatch.setenv("OP_HOOKER__API_KEY", "hk_env")
    monkeypatch.setenv("OP_HOOKER__TOPIC", "voice_agent")

    hooker = Config().hooker

    assert hooker.enabled is True
    assert hooker.url == "https://hooker.example"
    assert hooker.api_key == "hk_env"
    assert hooker.topic == "voice_agent"


def _enabled_config() -> HookerConfig:
    return HookerConfig(
        enabled=True,
        url="https://hooker.test",
        api_key="hk_test",
        topic="agent_in",
    )


def _unexpected_request(request: httpx.Request) -> httpx.Response:
    raise AssertionError(f"unexpected request to {request.url}")
