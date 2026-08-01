"""whisper.cpp local transcription provider.

Runs the whisper.cpp CLI (`whisper-cli`) as a subprocess: fully local, no cloud
key, and no new Python dependencies. Self-hosters install whisper.cpp however
they like (brew/apt/source), provide a ggml/gguf model file, and select this
provider with `OP_PIPELINE__TRANSCRIPTION=whisper_cpp`.
"""
from __future__ import annotations

import asyncio
import json
import os
import shlex
import shutil
import tempfile
from asyncio.subprocess import PIPE
from collections.abc import Sequence
from pathlib import Path

from open_plaud.transcription.base import register
from open_plaud.types import AudioRef, TranscribeOptions, Transcript, TranscriptSegment

_BIN_ENV = "OP_WHISPER_CPP_BIN"
_MODEL_ENV = "OP_WHISPER_CPP_MODEL"
_ARGS_ENV = "OP_WHISPER_CPP_ARGS"
_TIMEOUT_ENV = "OP_WHISPER_CPP_TIMEOUT_SECONDS"
_DEFAULT_BIN = "whisper-cli"
_DEFAULT_TIMEOUT_SECONDS = 900.0
_STDERR_TAIL = 2000
_SNIPPET_LEN = 200


def _snippet(raw: str) -> str:
    return repr(raw[:_SNIPPET_LEN] + ("..." if len(raw) > _SNIPPET_LEN else ""))


def _offset_seconds(value: object) -> float | None:
    if isinstance(value, int | float):
        return float(value) / 1000.0
    return None


def _parse_transcription(raw: str) -> tuple[str, list[TranscriptSegment]]:
    """Extract transcript text and segment timing from whisper.cpp `-oj` output."""
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"whisper_cpp: output file is not valid JSON ({exc}): {_snippet(raw)}"
        ) from exc

    raw_segments = payload.get("transcription") if isinstance(payload, dict) else None
    if not isinstance(raw_segments, list):
        raise RuntimeError(
            "whisper_cpp: unexpected JSON shape (expected a top-level "
            f"'transcription' list): {_snippet(raw)}"
        )

    segments: list[TranscriptSegment] = []
    for item in raw_segments:
        text = item.get("text") if isinstance(item, dict) else None
        if not isinstance(text, str):
            raise RuntimeError(
                "whisper_cpp: unexpected segment shape (expected {'text': str}): "
                f"{_snippet(raw)}"
            )
        offsets = item.get("offsets") if isinstance(item, dict) else None
        start = end = None
        if isinstance(offsets, dict):
            start = _offset_seconds(offsets.get("from"))
            end = _offset_seconds(offsets.get("to"))
        stripped = text.strip()
        if stripped:
            segments.append(TranscriptSegment(text=stripped, start=start, end=end))

    return " ".join(segment.text for segment in segments), segments


@register
class WhisperCppTranscriber:
    name = "whisper_cpp"

    def __init__(
        self,
        bin_path: str | Path | None = None,
        model_path: str | Path | None = None,
        *,
        binary: str | Path | None = None,
        model: str | Path | None = None,
        timeout_seconds: float | None = None,
        extra_args: Sequence[str] | None = None,
    ) -> None:
        configured_bin = str(binary or bin_path or os.environ.get(_BIN_ENV) or _DEFAULT_BIN)
        resolved_bin = shutil.which(configured_bin)
        if resolved_bin is None:
            raise RuntimeError(
                f"whisper_cpp: no executable whisper.cpp CLI at {configured_bin!r}. "
                f"Install it (e.g. `brew install whisper-cpp`) or set {_BIN_ENV} "
                "to the binary's path."
            )
        self._bin = resolved_bin

        configured_model = model or model_path or os.environ.get(_MODEL_ENV)
        if not configured_model:
            raise RuntimeError(
                f"{_MODEL_ENV} is required for the whisper_cpp transcription "
                "provider: point it at a ggml/gguf model file (download one from "
                "https://huggingface.co/ggerganov/whisper.cpp)."
            )
        model_file = Path(configured_model)
        if not model_file.is_file():
            raise RuntimeError(
                f"whisper_cpp: {_MODEL_ENV} model file not found: {model_file}. "
                "Download a ggml/gguf model from "
                "https://huggingface.co/ggerganov/whisper.cpp."
            )
        self._model = str(model_file)

        raw_timeout = os.environ.get(_TIMEOUT_ENV)
        self._timeout_seconds = (
            timeout_seconds
            if timeout_seconds is not None
            else float(raw_timeout or _DEFAULT_TIMEOUT_SECONDS)
        )

        if extra_args is None:
            extra_args = shlex.split(os.environ.get(_ARGS_ENV, ""))
        self._extra_args = tuple(extra_args)

    @staticmethod
    def _require_wav(audio: AudioRef) -> None:
        """Fail with a useful message when handed audio whisper.cpp cannot read.

        The CLI accepts PCM WAV only. Ingest decodes the device's ADPCM container
        to WAV, but Ogg Opus is passed through deliberately (the server takes no
        ffmpeg dependency), so an Opus upload reaches here intact and whisper.cpp
        rejects it with an opaque error. Say what is actually wrong instead.

        Checked by magic, not by suffix: the recorder's habit of naming non-WAV
        payloads `.wav` is exactly why this project does not trust suffixes.
        """
        try:
            with audio.path.open("rb") as handle:
                head = handle.read(12)
        except OSError as exc:
            raise RuntimeError(f"whisper_cpp: cannot read {audio.path}: {exc}") from exc
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise RuntimeError(
                f"whisper_cpp: {audio.filename} is not a PCM WAV file "
                f"(starts with {head[:4]!r}), and the whisper.cpp CLI reads "
                "nothing else. Ogg Opus uploads are stored as-is: transcribe "
                "them with a cloud provider, or turn the firmware's Opus "
                "sidecar off so clips arrive as Open Plaud ADPCM, which the "
                "server decodes to WAV at ingest."
            )

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        self._require_wav(audio)
        with tempfile.TemporaryDirectory(prefix="open-plaud-whisper-cpp-") as tmpdir:
            out_base = Path(tmpdir) / audio.path.stem
            cmd = [
                self._bin,
                "-m",
                self._model,
                "-f",
                str(audio.path),
                "-oj",
                "-of",
                str(out_base),
                "-np",
            ]
            if opts.language:
                cmd += ["-l", opts.language]
            cmd.extend(self._extra_args)

            proc = await asyncio.create_subprocess_exec(*cmd, stdout=PIPE, stderr=PIPE)
            try:
                _stdout, stderr = await asyncio.wait_for(
                    proc.communicate(), timeout=self._timeout_seconds
                )
            except asyncio.TimeoutError as exc:
                proc.kill()
                await proc.communicate()
                raise RuntimeError(
                    f"whisper_cpp transcription timed out after {self._timeout_seconds:g}s"
                ) from exc

            if proc.returncode != 0:
                tail = stderr.decode(errors="replace")[-_STDERR_TAIL:]
                raise RuntimeError(
                    f"whisper_cpp: {self._bin} exited with code {proc.returncode}: {tail}"
                )

            json_path = Path(f"{out_base}.json")
            try:
                raw = await asyncio.to_thread(json_path.read_text, "utf-8", "replace")
            except FileNotFoundError:
                tail = stderr.decode(errors="replace")[-_STDERR_TAIL:]
                raise RuntimeError(
                    f"whisper_cpp: CLI exited 0 but produced no JSON at {json_path}; "
                    f"stderr: {tail!r}"
                ) from None

        text, segments = _parse_transcription(raw)
        if not text:
            raise RuntimeError("whisper_cpp produced no transcript text")
        return Transcript(text=text, language=opts.language, segments=segments)
