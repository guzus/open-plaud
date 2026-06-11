from __future__ import annotations

import json
from pathlib import Path

import pytest

from open_plaud.transcription.whisper_cpp import WhisperCppTranscriber
from open_plaud.types import AudioRef, TranscribeOptions, Transcript


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


# What `whisper-cli -oj` actually writes (abridged): segment texts come with
# leading whitespace, plus top-level keys the parser must ignore.
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
            {"text": "  world. "},
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
    """Build a fake whisper-cli executable.

    Mimics the real CLI's contract: records its argv (one per line) to
    `<tmp_path>/args.txt` and, when given a payload, writes it to
    `<value after -of>.json` — whisper.cpp appends the `.json` extension
    itself. This exercises the real subprocess + JSON parsing path without
    whisper.cpp installed.
    """
    args_file = tmp_path / "args.txt"
    lines = [
        "#!/bin/sh",
        f"printf '%s\\n' \"$@\" > '{args_file}'",
        'out=""',
        'prev=""',
        'for a in "$@"; do',
        '  if [ "$prev" = "-of" ]; then out="$a"; fi',
        '  prev="$a"',
        "done",
    ]
    if stderr_line is not None:
        lines.append(f"echo '{stderr_line}' >&2")
    if payload is not None:
        payload_file = tmp_path / "payload.json"
        payload_file.write_text(payload)
        lines.append(f"cp '{payload_file}' \"$out.json\"")
    lines.append(f"exit {exit_code}")
    script = tmp_path / "fake-whisper-cli"
    script.write_text("\n".join(lines) + "\n")
    script.chmod(0o755)
    return script


async def test_transcribe_runs_cli_and_joins_segment_texts(tmp_path: Path) -> None:
    model = _model(tmp_path)
    audio = _audio(tmp_path)
    provider = WhisperCppTranscriber(bin_path=_fake_cli(tmp_path), model_path=model)

    result = await provider.transcribe(audio, TranscribeOptions())

    assert result == Transcript(text="Hello world.", language=None)
    args = (tmp_path / "args.txt").read_text().splitlines()
    assert args[args.index("-m") + 1] == str(model)
    assert args[args.index("-f") + 1] == str(audio.path)
    assert "-oj" in args  # JSON output requested
    assert "-np" in args  # keep the CLI quiet
    assert "-l" not in args  # no language requested
    out_base = args[args.index("-of") + 1]
    # The JSON went to a per-call temp dir (not the audio dir) and was cleaned up.
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
    # The registry instantiates providers with no args; everything comes from env.
    monkeypatch.setenv("OP_WHISPER_CPP_BIN", str(_fake_cli(tmp_path)))
    monkeypatch.setenv("OP_WHISPER_CPP_MODEL", str(_model(tmp_path)))

    provider = WhisperCppTranscriber()
    result = await provider.transcribe(_audio(tmp_path), TranscribeOptions())

    assert result.text == "Hello world."


async def test_missing_binary_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("OP_WHISPER_CPP_BIN", raising=False)
    monkeypatch.setenv("PATH", str(tmp_path))  # no whisper-cli anywhere on PATH
    with pytest.raises(RuntimeError, match="OP_WHISPER_CPP_BIN"):
        WhisperCppTranscriber(model_path=_model(tmp_path))


async def test_nonexecutable_bin_path_raises(tmp_path: Path) -> None:
    with pytest.raises(RuntimeError, match="brew install whisper-cpp"):
        WhisperCppTranscriber(
            bin_path=tmp_path / "missing-whisper-cli", model_path=_model(tmp_path)
        )


async def test_missing_model_env_raises(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("OP_WHISPER_CPP_MODEL", raising=False)
    with pytest.raises(RuntimeError, match="OP_WHISPER_CPP_MODEL"):
        WhisperCppTranscriber(bin_path=_fake_cli(tmp_path))


async def test_nonexistent_model_file_raises(tmp_path: Path) -> None:
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
    bin_path = _fake_cli(tmp_path, payload=None)  # exits 0 without writing JSON
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
