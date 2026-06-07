from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import StrEnum
from pathlib import Path


class JobStatus(StrEnum):
    RECEIVED = "received"
    TRANSCRIBING = "transcribing"
    DIARIZING = "diarizing"
    SUMMARIZING = "summarizing"
    DONE = "done"
    FAILED = "failed"


@dataclass(slots=True)
class AudioRef:
    path: Path
    filename: str
    content_type: str | None
    size_bytes: int


@dataclass(slots=True)
class TranscribeOptions:
    language: str | None = None
    diarize: bool = False


@dataclass(slots=True)
class Transcript:
    text: str


@dataclass(slots=True)
class Recording:
    id: str
    audio: AudioRef
    status: JobStatus = JobStatus.RECEIVED
    created_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    transcript: Transcript | None = None
    diarization: object | None = None
    summary: object | None = None
    error: str | None = None
