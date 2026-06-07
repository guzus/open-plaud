from __future__ import annotations

from open_plaud.transcription.base import register
from open_plaud.types import AudioRef, TranscribeOptions, Transcript


@register
class NoopTranscriber:
    name = "none"

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        return Transcript(text="")
