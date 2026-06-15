"""Local Ollama summarization provider."""

from __future__ import annotations

import json
import os
from typing import Any

import httpx

from open_plaud.summarization.base import register, transcript_user_message
from open_plaud.types import ActionItem, Summary, Transcript

_DEFAULT_BASE_URL = "http://127.0.0.1:11434"
_REQUEST_TIMEOUT = 120.0

_SYSTEM_PROMPT = (
    "Summarize an already-transcribed voice recording as JSON only. Return an "
    "object with a string `summary` field and an `action_items` array. Each action "
    "item must be an object with `text`, `owner`, and `due` fields; use null when "
    "owner or due date is not stated. Never ask for audio or claim that no audio "
    "or transcript was attached; if the transcript has no usable speech, state "
    "that no usable speech was transcribed."
)


def _nullable_string(value: object) -> str | None:
    if value is None:
        return None
    if isinstance(value, str):
        return value
    raise RuntimeError(f"expected string or null, got {value!r}")


def _parse_summary(raw: str) -> Summary:
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Ollama returned non-JSON summary: {raw!r}") from exc

    if not isinstance(data, dict):
        raise RuntimeError(f"Ollama summary must be a JSON object: {data!r}")
    summary = data.get("summary")
    if not isinstance(summary, str):
        raise RuntimeError(f"Ollama summary missing string `summary`: {data!r}")

    raw_items = data.get("action_items", [])
    if not isinstance(raw_items, list):
        raise RuntimeError(f"Ollama `action_items` must be a list: {data!r}")

    action_items: list[ActionItem] = []
    for item in raw_items:
        if not isinstance(item, dict):
            raise RuntimeError(f"Ollama action item must be an object: {item!r}")
        text = item.get("text")
        if not isinstance(text, str):
            raise RuntimeError(f"Ollama action item missing string `text`: {item!r}")
        action_items.append(
            ActionItem(
                text=text,
                owner=_nullable_string(item.get("owner")),
                due=_nullable_string(item.get("due")),
            )
        )

    return Summary(summary=summary, action_items=action_items)


@register
class OllamaSummarizer:
    name = "ollama"

    def __init__(
        self,
        *,
        model: str | None = None,
        base_url: str | None = None,
        http_client: httpx.AsyncClient | None = None,
    ) -> None:
        model = model or os.environ.get("OP_OLLAMA_MODEL")
        if not model:
            raise RuntimeError("OP_OLLAMA_MODEL is required for the ollama summarization provider")
        self._model = model
        self._base_url = (base_url or os.environ.get("OP_OLLAMA_URL") or _DEFAULT_BASE_URL).rstrip(
            "/"
        )
        self._http_client = http_client

    async def summarize(
        self, transcript: Transcript, *, instructions: str | None = None
    ) -> Summary:
        content = transcript_user_message(transcript, instructions=instructions)

        payload: dict[str, Any] = {
            "model": self._model,
            "stream": False,
            "format": "json",
            "messages": [
                {"role": "system", "content": _SYSTEM_PROMPT},
                {"role": "user", "content": content},
            ],
        }

        if self._http_client is not None:
            response = await self._http_client.post("/api/chat", json=payload)
        else:
            async with httpx.AsyncClient(
                base_url=self._base_url, timeout=_REQUEST_TIMEOUT
            ) as client:
                response = await client.post("/api/chat", json=payload)

        if not response.is_success:
            raise RuntimeError(
                f"Ollama summarization failed (HTTP {response.status_code}): {response.text}"
            )

        data = response.json()
        message = data.get("message")
        if not isinstance(message, dict) or not isinstance(message.get("content"), str):
            raise RuntimeError(f"Ollama response missing message.content: {data!r}")
        return _parse_summary(message["content"])
