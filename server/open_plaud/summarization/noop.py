from __future__ import annotations

from open_plaud.summarization.base import register
from open_plaud.types import Summary, Transcript


@register
class NoopSummarizer:
    name = "none"

    async def summarize(
        self, transcript: Transcript, *, instructions: str | None = None
    ) -> Summary:
        return Summary(summary=transcript.text, action_items=[])
