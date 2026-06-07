"""Transcription providers.

The whole "cloud now, local later" decision lives behind this Protocol. The
orchestrator depends only on `TranscriptionProvider`; concrete backends register
themselves under a name and are resolved from config.

Today: cloud providers (OpenAI Whisper API, etc).
Later: `whisper_cpp`, on-device mobile — added as new registrations, no caller change.
"""
from __future__ import annotations

from typing import Any, Protocol, cast, runtime_checkable

from open_plaud.types import AudioRef, TranscribeOptions, Transcript


@runtime_checkable
class TranscriptionProvider(Protocol):
    name: str

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        ...


_REGISTRY: dict[str, type[TranscriptionProvider]] = {}


def register(cls: type[TranscriptionProvider]) -> type[TranscriptionProvider]:
    """Class decorator: register a provider under its `name`."""
    name = getattr(cls, "name", None)
    if not name:
        raise ValueError(f"{cls.__name__} must define a class-level `name`")
    if name in _REGISTRY:
        raise ValueError(f"transcription provider '{name}' already registered")
    _REGISTRY[name] = cls
    return cls


def available() -> list[str]:
    return sorted(_REGISTRY)


def create(name: str, **kwargs: object) -> TranscriptionProvider:
    if name not in _REGISTRY:
        raise KeyError(
            f"unknown transcription provider '{name}'. "
            f"available: {', '.join(available()) or '(none)'}"
        )
    provider_cls = cast(type[Any], _REGISTRY[name])
    return cast(TranscriptionProvider, provider_cls(**kwargs))
