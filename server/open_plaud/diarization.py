from __future__ import annotations

import asyncio
import os
import tempfile
from asyncio.subprocess import PIPE
from collections.abc import Sequence
from importlib import import_module
from pathlib import Path
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


class PyannoteDiarizer:
    name = "pyannote"

    def __init__(
        self,
        *,
        model: str | None = None,
        auth_token: str | None = None,
        pipeline: Any | None = None,
    ) -> None:
        self._model = model or os.environ.get(
            "OP_PYANNOTE_MODEL", "pyannote/speaker-diarization-3.1"
        )
        self._auth_token = auth_token or os.environ.get("HUGGINGFACE_TOKEN")
        self._pipeline = pipeline

    def _load_pipeline(self) -> Any:
        if self._pipeline is None:
            try:
                module = import_module("pyannote.audio")
            except ImportError as exc:
                raise RuntimeError(
                    "pyannote.audio is required for the pyannote diarization provider"
                ) from exc
            pipeline_cls = getattr(module, "Pipeline")
            self._pipeline = pipeline_cls.from_pretrained(
                self._model, use_auth_token=self._auth_token
            )
        return self._pipeline

    async def diarize(self, audio: AudioRef) -> object:
        pipeline = self._load_pipeline()
        annotation = await asyncio.to_thread(pipeline, str(audio.path))
        segments: list[dict[str, object]] = []
        for turn, _track, speaker in annotation.itertracks(yield_label=True):
            segments.append(
                {
                    "start": float(turn.start),
                    "end": float(turn.end),
                    "speaker": str(speaker),
                }
            )
        return {"segments": segments}


class WhisperXDiarizer:
    name = "whisperx"

    def __init__(
        self,
        *,
        binary: str | None = None,
        model: str | None = None,
        auth_token: str | None = None,
        timeout_seconds: float | None = None,
        extra_args: Sequence[str] = (),
    ) -> None:
        self._binary = binary or os.environ.get("OP_WHISPERX_BIN", "whisperx")
        self._model = model or os.environ.get("OP_WHISPERX_MODEL", "base")
        self._auth_token = auth_token or os.environ.get("HUGGINGFACE_TOKEN")
        raw_timeout = os.environ.get("OP_WHISPERX_TIMEOUT_SECONDS")
        self._timeout_seconds = timeout_seconds if timeout_seconds is not None else float(
            raw_timeout or 1800.0
        )
        self._extra_args = tuple(extra_args)

    async def diarize(self, audio: AudioRef) -> object:
        with tempfile.TemporaryDirectory(prefix="open_plaud_whisperx_") as tmp:
            command = [
                self._binary,
                str(audio.path),
                "--model",
                self._model,
                "--diarize",
                "--output_format",
                "json",
                "--output_dir",
                tmp,
            ]
            if self._auth_token:
                command.extend(["--hf_token", self._auth_token])
            command.extend(self._extra_args)

            proc = await asyncio.create_subprocess_exec(
                *command,
                stdout=PIPE,
                stderr=PIPE,
            )
            try:
                stdout, stderr = await asyncio.wait_for(
                    proc.communicate(), timeout=self._timeout_seconds
                )
            except asyncio.TimeoutError as exc:
                proc.kill()
                await proc.communicate()
                raise RuntimeError(
                    f"whisperx diarization timed out after {self._timeout_seconds:g}s"
                ) from exc

            if proc.returncode != 0:
                detail = (stderr or stdout).decode("utf-8", errors="replace").strip()
                raise RuntimeError(
                    f"whisperx diarization failed with exit code {proc.returncode}: {detail}"
                )

            json_files = sorted(Path(tmp).glob("*.json"))
            if not json_files:
                raise RuntimeError("whisperx produced no JSON output")
            return _normalize_whisperx_json(json_files[0])


def _normalize_whisperx_json(path: Path) -> object:
    import json

    data = json.loads(path.read_text(encoding="utf-8"))
    raw_segments = data.get("segments") if isinstance(data, dict) else None
    if not isinstance(raw_segments, list):
        raise RuntimeError(f"whisperx JSON has no segments list: {data!r}")

    segments: list[dict[str, object]] = []
    for item in raw_segments:
        if not isinstance(item, dict):
            continue
        start = item.get("start")
        end = item.get("end")
        speaker = item.get("speaker")
        text = item.get("text")
        if not isinstance(start, int | float) or not isinstance(end, int | float):
            continue
        normalized: dict[str, object] = {"start": float(start), "end": float(end)}
        if isinstance(speaker, str):
            normalized["speaker"] = speaker
        if isinstance(text, str):
            normalized["text"] = text
        segments.append(normalized)
    return {"segments": segments}


_REGISTRY: dict[str, type[DiarizationProvider]] = {
    "none": NoopDiarizer,
    "pyannote": PyannoteDiarizer,
    "whisperx": WhisperXDiarizer,
}


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
