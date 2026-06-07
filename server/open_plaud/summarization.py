from __future__ import annotations

from typing import Any, Protocol, cast, runtime_checkable

from open_plaud.types import Transcript


@runtime_checkable
class SummarizationProvider(Protocol):
    name: str

    async def summarize(self, transcript: Transcript, *, instructions: str | None = None) -> object:
        ...


class NoopSummarizer:
    name = "none"

    async def summarize(self, transcript: Transcript, *, instructions: str | None = None) -> object:
        return {"summary": transcript.text, "action_items": []}


_REGISTRY: dict[str, type[SummarizationProvider]] = {"none": NoopSummarizer}


def register(cls: type[SummarizationProvider]) -> type[SummarizationProvider]:
    name = getattr(cls, "name", None)
    if not name:
        raise ValueError(f"{cls.__name__} must define a class-level `name`")
    if name in _REGISTRY:
        raise ValueError(f"summarization provider '{name}' already registered")
    _REGISTRY[name] = cls
    return cls


def available() -> list[str]:
    return sorted(_REGISTRY)


def create(name: str, **kwargs: object) -> SummarizationProvider:
    if name not in _REGISTRY:
        raise KeyError(
            f"unknown summarization provider '{name}'. "
            f"available: {', '.join(available()) or '(none)'}"
        )
    provider_cls = cast(type[Any], _REGISTRY[name])
    return cast(SummarizationProvider, provider_cls(**kwargs))
