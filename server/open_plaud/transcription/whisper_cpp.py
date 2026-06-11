"""whisper.cpp local transcription provider.

Runs the whisper.cpp CLI (`whisper-cli`) as a subprocess: fully local, no cloud
key, and no new Python dependencies — self-hosters install whisper.cpp however
they like (brew/apt/source) and point the env vars below at it. whisper.cpp
natively reads the 16 kHz PCM WAV the firmware records, so audio is passed
straight through. Registered under the name `whisper_cpp`; the registry
instantiates it with no args, so config is resolved from the environment in
`__init__` — lazily (importing this module is what registers the provider, so
nothing is read at import time) and fail-fast, like the other providers.

Env:
    OP_WHISPER_CPP_BIN    path to the whisper.cpp CLI. Default: `whisper-cli`
                          on PATH (the binary's post-2024 name; older builds
                          called it `main`).
    OP_WHISPER_CPP_MODEL  path to a ggml/gguf model file (required) — download
                          from https://huggingface.co/ggerganov/whisper.cpp
"""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import tempfile
from pathlib import Path

from open_plaud.transcription.base import register
from open_plaud.types import AudioRef, TranscribeOptions, Transcript

_BIN_ENV = "OP_WHISPER_CPP_BIN"
_MODEL_ENV = "OP_WHISPER_CPP_MODEL"
_DEFAULT_BIN = "whisper-cli"
_STDERR_TAIL = 2000  # chars of stderr to keep in error messages
_SNIPPET_LEN = 200  # chars of unexpected output to quote in errors


def _snippet(raw: str) -> str:
    return repr(raw[:_SNIPPET_LEN] + ("..." if len(raw) > _SNIPPET_LEN else ""))


def _parse_transcription(raw: str) -> str:
    """Extract the transcript text from whisper.cpp's `-oj` output.

    Expected shape: ``{"transcription": [{"text": " ..."}, ...]}`` plus other
    top-level keys (systeminfo, model, params, result) that we ignore. Segment
    texts carry leading spaces, so each is stripped and they are joined with
    single spaces.
    """
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"whisper_cpp: output file is not valid JSON ({exc}): {_snippet(raw)}"
        ) from exc
    segments = payload.get("transcription") if isinstance(payload, dict) else None
    if not isinstance(segments, list):
        raise RuntimeError(
            "whisper_cpp: unexpected JSON shape (expected a top-level "
            f"'transcription' list): {_snippet(raw)}"
        )
    texts: list[str] = []
    for segment in segments:
        text = segment.get("text") if isinstance(segment, dict) else None
        if not isinstance(text, str):
            raise RuntimeError(
                "whisper_cpp: unexpected segment shape (expected {'text': str}): "
                f"{_snippet(raw)}"
            )
        texts.append(text.strip())
    return " ".join(text for text in texts if text)


@register
class WhisperCppTranscriber:
    name = "whisper_cpp"

    def __init__(
        self,
        bin_path: str | Path | None = None,
        model_path: str | Path | None = None,
    ) -> None:
        # Resolve config now and fail fast: the pipeline wires providers once at
        # startup, so a missing binary/model should blow up loudly there, not
        # mid-request. `shutil.which` covers both forms at once: bare names get
        # a PATH lookup, explicit paths an is-executable-file check.
        configured_bin = str(bin_path or os.environ.get(_BIN_ENV) or _DEFAULT_BIN)
        resolved_bin = shutil.which(configured_bin)
        if resolved_bin is None:
            raise RuntimeError(
                f"whisper_cpp: no executable whisper.cpp CLI at {configured_bin!r}. "
                f"Install it (e.g. `brew install whisper-cpp`) or set {_BIN_ENV} "
                "to the binary's path."
            )
        self._bin = resolved_bin

        configured_model = model_path or os.environ.get(_MODEL_ENV)
        if not configured_model:
            raise RuntimeError(
                f"{_MODEL_ENV} is required for the whisper_cpp transcription "
                "provider: point it at a ggml/gguf model file (download one from "
                "https://huggingface.co/ggerganov/whisper.cpp)."
            )
        model = Path(configured_model)
        if not model.is_file():
            raise RuntimeError(
                f"whisper_cpp: {_MODEL_ENV} model file not found: {model}. Download "
                "a ggml/gguf model from https://huggingface.co/ggerganov/whisper.cpp."
            )
        self._model = str(model)

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        # The JSON goes to a per-call temp dir: parallel transcriptions never
        # collide and nothing is left next to the audio. `-of` takes the output
        # basename *without* the extension; the CLI appends `.json` itself.
        with tempfile.TemporaryDirectory(prefix="open-plaud-whisper-cpp-") as tmpdir:
            out_base = Path(tmpdir) / audio.path.stem
            cmd = [
                self._bin,
                "-m", self._model,
                "-f", str(audio.path),
                "-oj",
                "-of", str(out_base),
                "-np",
            ]
            if opts.language:
                cmd += ["-l", opts.language]

            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            _stdout, stderr = await proc.communicate()
            if proc.returncode != 0:
                tail = stderr.decode(errors="replace")[-_STDERR_TAIL:]
                raise RuntimeError(
                    f"whisper_cpp: {self._bin} exited with code {proc.returncode}: {tail}"
                )

            # Str concat, not with_suffix(): stems may already contain dots.
            json_path = Path(f"{out_base}.json")
            try:
                # Read off the event loop: file I/O is blocking. errors="replace"
                # so malformed bytes surface as a parse error, not a decode crash.
                raw = await asyncio.to_thread(json_path.read_text, "utf-8", "replace")
            except FileNotFoundError:
                tail = stderr.decode(errors="replace")[-_STDERR_TAIL:]
                raise RuntimeError(
                    f"whisper_cpp: CLI exited 0 but produced no JSON at {json_path}; "
                    f"stderr: {tail!r}"
                ) from None

        return Transcript(text=_parse_transcription(raw), language=opts.language)
