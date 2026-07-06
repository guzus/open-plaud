from __future__ import annotations

from dataclasses import dataclass
from urllib.parse import quote

import httpx

from open_plaud.config import HookerConfig
from open_plaud.types import Recording, Summary


class HookerDispatchError(RuntimeError):
    """Raised when an enabled Hooker dispatch cannot be completed."""


@dataclass(slots=True)
class HookerDispatchResult:
    published: bool
    reason: str | None = None


class HookerDispatcher:
    def __init__(
        self,
        config: HookerConfig,
        http_client: httpx.AsyncClient | None = None,
    ) -> None:
        self.config = config
        self._http_client = http_client

    async def dispatch(self, rec: Recording) -> HookerDispatchResult:
        if not self.config.enabled:
            return HookerDispatchResult(published=False, reason="disabled")
        self._validate_config()
        transcript = rec.transcript.text.strip() if rec.transcript is not None else ""
        if not transcript:
            return HookerDispatchResult(published=False, reason="empty_transcript")

        payload = {
            "title": f"Voice prompt: {rec.audio.filename}",
            "text": _build_prompt(rec, prompt_footer=self.config.prompt_footer),
            "priority": self.config.priority,
            "tags": _parse_tags(self.config.tags),
        }
        headers = {
            "Authorization": f"Bearer {self.config.api_key}",
            "Content-Type": "application/json",
            "Idempotency-Key": f"open-plaud:recording:{rec.id}:hooker:v1",
        }

        client = self._http_client
        if client is not None:
            response = await client.post(_publish_url(self.config), json=payload, headers=headers)
        else:
            async with httpx.AsyncClient(timeout=self.config.timeout_seconds) as fresh_client:
                response = await fresh_client.post(
                    _publish_url(self.config), json=payload, headers=headers
                )

        if response.status_code < 200 or response.status_code >= 300:
            raise HookerDispatchError(
                f"Hooker publish failed with status {response.status_code}: {response.text}"
            )
        return HookerDispatchResult(published=True)

    def _validate_config(self) -> None:
        if not self.config.url.strip():
            raise HookerDispatchError("OP_HOOKER__URL is required when Hooker dispatch is enabled")
        if not self.config.api_key.strip():
            raise HookerDispatchError(
                "OP_HOOKER__API_KEY is required when Hooker dispatch is enabled"
            )
        if not self.config.topic.strip():
            raise HookerDispatchError("OP_HOOKER__TOPIC is required when Hooker dispatch is enabled")


def _publish_url(config: HookerConfig) -> str:
    return f"{config.url.rstrip('/')}/publish/{quote(config.topic.strip(), safe='')}"


def _parse_tags(raw: str) -> list[str]:
    tags: list[str] = []
    seen: set[str] = set()
    for tag in raw.split(","):
        cleaned = tag.strip()
        key = cleaned.lower()
        if cleaned and key not in seen:
            tags.append(cleaned)
            seen.add(key)
    return tags


def _build_prompt(rec: Recording, *, prompt_footer: str = "") -> str:
    transcript = rec.transcript.text.strip() if rec.transcript is not None else ""
    lines = [
        "Voice prompt from open-plaud.",
        "",
        f"Recording id: {rec.id}",
        f"Filename: {rec.audio.filename}",
        "",
        "Transcript:",
        transcript,
    ]

    if isinstance(rec.summary, Summary) and rec.summary.summary.strip():
        lines += ["", "Summary:", rec.summary.summary.strip()]
        if rec.summary.action_items:
            lines += ["", "Action items extracted from the recording:"]
            for item in rec.summary.action_items:
                owner = f" owner={item.owner}" if item.owner else ""
                due = f" due={item.due}" if item.due else ""
                lines.append(f"- {item.text}{owner}{due}")

    footer = prompt_footer.strip()
    if footer:
        lines += ["", footer]
    return "\n".join(lines)
