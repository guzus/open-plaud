from __future__ import annotations

import json
import shlex
from pathlib import Path

import pytest

from open_plaud import transcription
from open_plaud.transcription.whisper_cpp import WhisperCppTranscriber
from open_plaud.types import AudioRef, TranscribeOptions


def _audio(tmp_path: Path, content: bytes = b"RIFF....WAVEfmt ") -> AudioRef:
    path = tmp_path / "rec.wav"
    path.write_bytes(content)
    return AudioRef(
        path=path,
        filename="rec.wav",
        content_type="audio/wav",
        size_bytes=len(content),
    )


def _model(tmp_path: Path) -> Path:
    model = tmp_path / "ggml-base.en.bin"
    model.write_bytes(b"ggml")
    return model


_PAYLOAD = json.dumps(
    {
        "systeminfo": "AVX = 1 | NEON = 1 |",
        "model": {"type": "base"},
        "params": {"model": "ggml-base.en.bin"},
        "result": {"language": "en"},
        "transcription": [
            {
                "timestamps": {"from": "00:00:00,000", "to": "00:00:02,000"},
                "offsets": {"from": 0, "to": 2000},
                "text": " Hello",
            },
            {"offsets": {"from": 2000, "to": 3000}, "text": "  world. "},
        ],
    }
)


def _fake_cli(
    tmp_path: Path,
    *,
    payload: str | None = _PAYLOAD,
    exit_code: int = 0,
    stderr_line: str | None = None,
) -> Path:
    args_file = tmp_path / "args.txt"
    lines = [
        "#!/bin/sh",
        f"printf '%s\\n' \"$@\" > {shlex.quote(str(args_file))}",
        'out=""',
        'prev=""',
        'for a in "$@"; do',
        '  if [ "$prev" = "-of" ]; then out="$a"; fi',
        '  prev="$a"',
        "done",
    ]
    if stderr_line is not None:
        lines.append(f"printf '%s\\n' {shlex.quote(stderr_line)} >&2")
    if payload is not None:
        payload_file = tmp_path / "payload.json"
        payload_file.write_text(payload)
        lines.append(f"cp {shlex.quote(str(payload_file))} \"$out.json\"")
    lines.append(f"exit {exit_code}")
    script = tmp_path / "fake-whisper-cli"
    script.write_text("\n".join(lines) + "\n")
    script.chmod(0o755)
    return script


async def test_transcribe_runs_cli_and_joins_segment_texts(tmp_path: Path) -> None:
    model = _model(tmp_path)
    audio = _audio(tmp_path)
    provider = WhisperCppTranscriber(
        bin_path=_fake_cli(tmp_path),
        model_path=model,
        timeout_seconds=5,
        extra_args=("--best-of", "1"),
    )

    result = await provider.transcribe(audio, TranscribeOptions())

    assert result.text == "Hello world."
    assert result.language is None
    assert result.segments[0].text == "Hello"
    assert result.segments[0].start == 0.0
    assert result.segments[0].end == 2.0
    assert result.segments[1].text == "world."
    assert result.segments[1].start == 2.0
    assert result.segments[1].end == 3.0
    args = (tmp_path / "args.txt").read_text().splitlines()
    assert args[args.index("-m") + 1] == str(model)
    assert args[args.index("-f") + 1] == str(audio.path)
    assert "-oj" in args
    assert "-np" in args
    assert "-l" not in args
    assert ["--best-of", "1"] == args[-2:]
    out_base = args[args.index("-of") + 1]
    assert Path(out_base).parent != audio.path.parent
    assert not Path(f"{out_base}.json").exists()


async def test_language_flag_forwarded_when_set(tmp_path: Path) -> None:
    provider = WhisperCppTranscriber(
        bin_path=_fake_cli(tmp_path), model_path=_model(tmp_path)
    )

    result = await provider.transcribe(_audio(tmp_path), TranscribeOptions(language="ko"))

    assert result.language == "ko"
    args = (tmp_path / "args.txt").read_text().splitlines()
    assert args[args.index("-l") + 1] == "ko"


async def test_config_resolved_from_env(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("OP_WHISPER_CPP_BIN", str(_fake_cli(tmp_path)))
    monkeypatch.setenv("OP_WHISPER_CPP_MODEL", str(_model(tmp_path)))
    monkeypatch.setenv("OP_WHISPER_CPP_ARGS", "--max-len 80")

    provider = WhisperCppTranscriber()
    result = await provider.transcribe(_audio(tmp_path), TranscribeOptions())

    assert result.text == "Hello world."
    args = (tmp_path / "args.txt").read_text().splitlines()
    assert ["--max-len", "80"] == args[-2:]


def test_legacy_constructor_names_still_work(tmp_path: Path) -> None:
    provider = WhisperCppTranscriber(
        binary=_fake_cli(tmp_path),
        model=_model(tmp_path),
        timeout_seconds=5,
    )
    assert provider.name == "whisper_cpp"


async def test_missing_binary_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("OP_WHISPER_CPP_BIN", raising=False)
    monkeypatch.setenv("PATH", str(tmp_path))
    with pytest.raises(RuntimeError, match="OP_WHISPER_CPP_BIN"):
        WhisperCppTranscriber(model_path=_model(tmp_path))


def test_nonexecutable_bin_path_raises(tmp_path: Path) -> None:
    with pytest.raises(RuntimeError, match="brew install whisper-cpp"):
        WhisperCppTranscriber(
            bin_path=tmp_path / "missing-whisper-cli", model_path=_model(tmp_path)
        )


def test_missing_model_env_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("OP_WHISPER_CPP_MODEL", raising=False)
    with pytest.raises(RuntimeError, match="OP_WHISPER_CPP_MODEL"):
        WhisperCppTranscriber(bin_path=_fake_cli(tmp_path))


def test_nonexistent_model_file_raises(tmp_path: Path) -> None:
    with pytest.raises(RuntimeError, match="OP_WHISPER_CPP_MODEL"):
        WhisperCppTranscriber(
            bin_path=_fake_cli(tmp_path), model_path=tmp_path / "no-such-model.bin"
        )


async def test_nonzero_exit_raises_with_stderr(tmp_path: Path) -> None:
    bin_path = _fake_cli(
        tmp_path, payload=None, exit_code=3, stderr_line="failed to load model"
    )
    provider = WhisperCppTranscriber(bin_path=bin_path, model_path=_model(tmp_path))
    with pytest.raises(RuntimeError, match="code 3.*failed to load model"):
        await provider.transcribe(_audio(tmp_path), TranscribeOptions())


async def test_missing_json_output_raises(tmp_path: Path) -> None:
    bin_path = _fake_cli(tmp_path, payload=None)
    provider = WhisperCppTranscriber(bin_path=bin_path, model_path=_model(tmp_path))
    with pytest.raises(RuntimeError, match="produced no JSON"):
        await provider.transcribe(_audio(tmp_path), TranscribeOptions())


async def test_garbage_json_raises_with_snippet(tmp_path: Path) -> None:
    bin_path = _fake_cli(tmp_path, payload="whisper.cpp went sideways")
    provider = WhisperCppTranscriber(bin_path=bin_path, model_path=_model(tmp_path))
    with pytest.raises(RuntimeError, match="not valid JSON.*went sideways"):
        await provider.transcribe(_audio(tmp_path), TranscribeOptions())


async def test_unexpected_json_shape_raises(tmp_path: Path) -> None:
    bin_path = _fake_cli(tmp_path, payload=json.dumps({"transcription": "not-a-list"}))
    provider = WhisperCppTranscriber(bin_path=bin_path, model_path=_model(tmp_path))
    with pytest.raises(RuntimeError, match="unexpected JSON shape"):
        await provider.transcribe(_audio(tmp_path), TranscribeOptions())


async def test_opus_upload_fails_with_an_actionable_message(tmp_path: Path) -> None:
    # Opus is passed through ingest untouched, so it can reach this provider —
    # which cannot read it. The error must say so rather than surfacing whatever
    # the CLI prints.
    opus = tmp_path / "rec.opus"
    opus.write_bytes(b"OggS" + b"\x00" * 32)
    audio = AudioRef(
        path=opus, filename="rec.opus", content_type="audio/ogg", size_bytes=36
    )
    provider = WhisperCppTranscriber(
        bin_path=_fake_cli(tmp_path), model_path=_model(tmp_path)
    )

    with pytest.raises(RuntimeError, match="not a PCM WAV file.*OggS"):
        await provider.transcribe(audio, TranscribeOptions())


async def test_adpcm_container_named_wav_is_rejected_by_magic(tmp_path: Path) -> None:
    # Belt and braces for the bug this branch is about: if an undecoded container
    # ever reaches transcription under its `.wav` name, fail loudly.
    audio = _audio(tmp_path, content=b"OPADPCM1" + b"\x00" * 14)
    provider = WhisperCppTranscriber(
        bin_path=_fake_cli(tmp_path), model_path=_model(tmp_path)
    )

    with pytest.raises(RuntimeError, match="not a PCM WAV file"):
        await provider.transcribe(audio, TranscribeOptions())


def test_package_wiring_exposes_whisper_cpp() -> None:
    assert "whisper_cpp" in transcription.available()
