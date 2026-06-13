from __future__ import annotations

import json

import httpx
import pytest

from open_plaud import summarization
from open_plaud.summarization.ollama import OllamaSummarizer
from open_plaud.types import Transcript


def _client_returning(content: str, status: int = 200) -> httpx.AsyncClient:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            status,
            json={"message": {"role": "assistant", "content": content}},
        )

    return httpx.AsyncClient(base_url="http://ollama.test", transport=httpx.MockTransport(handler))


async def test_ollama_summarizer_returns_summary_with_action_items() -> None:
    content = json.dumps(
        {
            "summary": "We discussed the launch.",
            "action_items": [
                {"text": "send launch notes", "owner": "Mina", "due": None}
            ],
        }
    )
    async with _client_returning(content) as client:
        summarizer = OllamaSummarizer(model="qwen", http_client=client)
        result = await summarizer.summarize(Transcript(text="launch transcript"))

    assert result.summary == "We discussed the launch."
    assert len(result.action_items) == 1
    assert result.action_items[0].text == "send launch notes"
    assert result.action_items[0].owner == "Mina"
    assert result.action_items[0].due is None


async def test_ollama_summarizer_sends_json_chat_request() -> None:
    captured: dict[str, object] = {}
    content = json.dumps({"summary": "Done.", "action_items": []})

    def handler(request: httpx.Request) -> httpx.Response:
        captured["path"] = request.url.path
        captured["body"] = json.loads(request.content)
        return httpx.Response(200, json={"message": {"content": content}})

    async with httpx.AsyncClient(
        base_url="http://ollama.test", transport=httpx.MockTransport(handler)
    ) as client:
        summarizer = OllamaSummarizer(model="llama", http_client=client)
        await summarizer.summarize(
            Transcript(text="hello"), instructions="Use concise Korean."
        )

    assert captured["path"] == "/api/chat"
    body = captured["body"]
    assert isinstance(body, dict)
    assert body["model"] == "llama"
    assert body["stream"] is False
    assert body["format"] == "json"
    assert "Use concise Korean." in body["messages"][1]["content"]


def test_ollama_summarizer_requires_model(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("OP_OLLAMA_MODEL", raising=False)

    with pytest.raises(RuntimeError, match="OP_OLLAMA_MODEL"):
        OllamaSummarizer()


async def test_ollama_summarizer_rejects_malformed_json() -> None:
    async with _client_returning("not json") as client:
        summarizer = OllamaSummarizer(model="qwen", http_client=client)

        with pytest.raises(RuntimeError, match="non-JSON"):
            await summarizer.summarize(Transcript(text="hello"))


async def test_ollama_summarizer_non_2xx_raises() -> None:
    async with _client_returning("{}", status=500) as client:
        summarizer = OllamaSummarizer(model="qwen", http_client=client)

        with pytest.raises(RuntimeError, match="HTTP 500"):
            await summarizer.summarize(Transcript(text="hello"))


def test_package_wiring_exposes_ollama() -> None:
    assert "ollama" in summarization.available()
