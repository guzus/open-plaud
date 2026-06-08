from __future__ import annotations

import os
from typing import Any

import httpx

from open_plaud.summarization.base import register
from open_plaud.types import ActionItem, Summary, Transcript

_API_URL = "https://api.anthropic.com/v1/messages"
_ANTHROPIC_VERSION = "2023-06-01"

_SYSTEM_PROMPT = (
    "You summarize a transcript of a voice recording. Write a concise summary "
    "capturing the key points and decisions, and extract any concrete action "
    "items (with owner and due date when stated). Respond ONLY by calling the "
    "record_summary tool — do not write any other text."
)

# Anthropic tool definition. Forcing structured output through a tool (with
# tool_choice pinned to it) is more robust across model tiers than asking for
# free-text JSON and parsing it.
RECORD_SUMMARY_TOOL: dict[str, Any] = {
    "name": "record_summary",
    "description": (
        "Record the summary of the voice recording transcript and any extracted "
        "action items."
    ),
    "input_schema": {
        "type": "object",
        "properties": {
            "summary": {
                "type": "string",
                "description": "A concise summary of the transcript.",
            },
            "action_items": {
                "type": "array",
                "description": "Concrete follow-up actions mentioned in the recording.",
                "items": {
                    "type": "object",
                    "properties": {
                        "text": {
                            "type": "string",
                            "description": "The action to be taken.",
                        },
                        "owner": {
                            "type": ["string", "null"],
                            "description": "Who is responsible, if stated.",
                        },
                        "due": {
                            "type": ["string", "null"],
                            "description": "When it is due, if stated.",
                        },
                    },
                    "required": ["text"],
                },
            },
        },
        "required": ["summary", "action_items"],
    },
}


@register
class ClaudeSummarizer:
    name = "claude"

    def __init__(
        self,
        api_key: str | None = None,
        model: str | None = None,
        http_client: httpx.AsyncClient | None = None,
    ) -> None:
        api_key = api_key or os.environ.get("ANTHROPIC_API_KEY")
        if not api_key:
            raise RuntimeError(
                "ANTHROPIC_API_KEY is required for the claude summarization provider"
            )
        self._api_key = api_key
        # Haiku 4.5 is the cheapest tier and is the right default for
        # per-recording summarization; override via OP_CLAUDE_MODEL to
        # claude-sonnet-4-6 or claude-opus-4-8 for higher quality.
        self._model = model or os.environ.get("OP_CLAUDE_MODEL", "claude-haiku-4-5")
        self._http_client = http_client

    async def summarize(
        self, transcript: Transcript, *, instructions: str | None = None
    ) -> Summary:
        content = transcript.text
        if instructions:
            content = f"{content}\n\nAdditional instructions: {instructions}"

        # Minimal body — no thinking/effort/temperature/top_p/top_k, which 400 on
        # some tiers. This keeps the request compatible across Haiku/Sonnet/Opus.
        payload: dict[str, Any] = {
            "model": self._model,
            "max_tokens": 2048,
            "system": _SYSTEM_PROMPT,
            "messages": [{"role": "user", "content": content}],
            "tools": [RECORD_SUMMARY_TOOL],
            "tool_choice": {"type": "tool", "name": "record_summary"},
        }
        headers = {
            "x-api-key": self._api_key,
            "anthropic-version": _ANTHROPIC_VERSION,
            "content-type": "application/json",
        }

        if self._http_client is not None:
            response = await self._http_client.post(
                _API_URL, json=payload, headers=headers
            )
        else:
            async with httpx.AsyncClient(timeout=120) as client:
                response = await client.post(_API_URL, json=payload, headers=headers)

        if not (200 <= response.status_code < 300):
            raise RuntimeError(
                f"Claude summarization request failed with status "
                f"{response.status_code}: {response.text}"
            )

        data = response.json()
        for block in data.get("content", []):
            if block.get("type") == "tool_use" and block.get("name") == "record_summary":
                inp = block.get("input", {})
                try:
                    return Summary(
                        summary=inp["summary"],
                        action_items=[
                            ActionItem(
                                text=item["text"],
                                owner=item.get("owner"),
                                due=item.get("due"),
                            )
                            for item in inp.get("action_items", [])
                        ],
                    )
                except (KeyError, TypeError) as exc:
                    raise RuntimeError(
                        f"Claude record_summary input was malformed: {inp!r}"
                    ) from exc
        raise RuntimeError(
            "Claude response contained no record_summary tool_use block"
        )
