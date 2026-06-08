from __future__ import annotations

import json

import httpx
import pytest

from open_plaud import summarization
from open_plaud.summarization.claude import ClaudeSummarizer
from open_plaud.summarization.noop import NoopSummarizer
from open_plaud.types import Transcript

_ANTHROPIC_BODY: dict[str, object] = {
    "id": "msg_1",
    "type": "message",
    "role": "assistant",
    "model": "claude-haiku-4-5",
    "stop_reason": "tool_use",
    "content": [
        {
            "type": "tool_use",
            "id": "toolu_1",
            "name": "record_summary",
            "input": {
                "summary": "We discussed X.",
                "action_items": [
                    {"text": "email Bob", "owner": "Alice", "due": None}
                ],
            },
        }
    ],
}


def _client_returning(body: dict[str, object], status: int = 200) -> httpx.AsyncClient:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(status, json=body)

    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def test_summarize_returns_summary_with_action_items() -> None:
    async with _client_returning(_ANTHROPIC_BODY) as client:
        summarizer = ClaudeSummarizer(api_key="test", http_client=client)
        result = await summarizer.summarize(Transcript(text="hello world"))

    assert result.summary == "We discussed X."
    assert len(result.action_items) == 1
    item = result.action_items[0]
    assert item.text == "email Bob"
    assert item.owner == "Alice"
    assert item.due is None


async def test_summarize_sends_forced_tool_choice() -> None:
    captured: dict[str, object] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        captured["body"] = json.loads(request.content)
        return httpx.Response(200, json=_ANTHROPIC_BODY)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        summarizer = ClaudeSummarizer(api_key="test", http_client=client)
        await summarizer.summarize(Transcript(text="hello world"))

    body = captured["body"]
    assert isinstance(body, dict)
    assert body["tool_choice"] == {"type": "tool", "name": "record_summary"}
    assert body["tools"][0]["name"] == "record_summary"
    # Minimal body: none of the tier-incompatible knobs should be present.
    for forbidden in ("thinking", "effort", "temperature", "top_p", "top_k"):
        assert forbidden not in body


def test_missing_api_key_raises(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    with pytest.raises(RuntimeError):
        ClaudeSummarizer()


async def test_non_2xx_raises_runtime_error() -> None:
    async with _client_returning({"error": "bad"}, status=400) as client:
        summarizer = ClaudeSummarizer(api_key="test", http_client=client)
        with pytest.raises(RuntimeError):
            await summarizer.summarize(Transcript(text="hello world"))


def test_package_wiring_exposes_providers() -> None:
    assert {"none", "claude"} <= set(summarization.available())


def test_create_none_returns_noop_summarizer() -> None:
    provider = summarization.create("none")
    assert isinstance(provider, NoopSummarizer)
    assert provider.name == "none"


def test_create_claude_works_with_api_key(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ANTHROPIC_API_KEY", "test")
    provider = summarization.create("claude")
    assert isinstance(provider, ClaudeSummarizer)
    assert provider.name == "claude"


async def test_noop_summarizer_passes_through_text() -> None:
    summarizer = NoopSummarizer()
    result = await summarizer.summarize(Transcript(text="just the text"))
    assert result.summary == "just the text"
    assert result.action_items == []
