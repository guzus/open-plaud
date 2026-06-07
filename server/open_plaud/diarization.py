from __future__ import annotations

from typing import Any, Protocol, cast, runtime_checkable

from open_plaud.types import AudioRef


@runtime_checkable
class DiarizationProvider(Protocol):
    name: str

    async def diarize(self, audio: AudioRef) -> object:
        ...


class NoopDiarizer:
    name = "none"

    async def diarize(self, audio: AudioRef) -> object:
        return {"segments": []}


_REGISTRY: dict[str, type[DiarizationProvider]] = {"none": NoopDiarizer}


def register(cls: type[DiarizationProvider]) -> type[DiarizationProvider]:
    name = getattr(cls, "name", None)
    if not name:
        raise ValueError(f"{cls.__name__} must define a class-level `name`")
    if name in _REGISTRY:
        raise ValueError(f"diarization provider '{name}' already registered")
    _REGISTRY[name] = cls
    return cls


def available() -> list[str]:
    return sorted(_REGISTRY)


def create(name: str, **kwargs: object) -> DiarizationProvider:
    if name not in _REGISTRY:
        raise KeyError(
            f"unknown diarization provider '{name}'. "
            f"available: {', '.join(available()) or '(none)'}"
        )
    provider_cls = cast(type[Any], _REGISTRY[name])
    return cast(DiarizationProvider, provider_cls(**kwargs))
