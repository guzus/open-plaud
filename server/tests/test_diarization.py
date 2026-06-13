from __future__ import annotations

import sys
import types
from pathlib import Path

import pytest

from open_plaud import diarization
from open_plaud.diarization import PyannoteDiarizer, WhisperXDiarizer
from open_plaud.pipeline.orchestrator import Pipeline, _merge_speakers
from open_plaud.types import AudioRef, JobStatus, Recording, Summary, Transcript, TranscriptSegment


def _audio(tmp_path: Path) -> AudioRef:
    path = tmp_path / "rec.wav"
    path.write_bytes(b"RIFF....WAVEfmt ")
    return AudioRef(
        path=path,
        filename="rec.wav",
        content_type="audio/wav",
        size_bytes=path.stat().st_size,
    )


def test_merge_speakers_uses_largest_overlap() -> None:
    transcript = Transcript(
        text="hello world",
        language="en",
        segments=[
            TranscriptSegment(text="hello", start=0.0, end=1.0),
            TranscriptSegment(text="world", start=1.0, end=2.0),
        ],
    )
    diarized = {
        "segments": [
            {"start": 0.0, "end": 1.2, "speaker": "SPEAKER_00"},
            {"start": 1.2, "end": 2.0, "speaker": "SPEAKER_01"},
        ]
    }

    merged = _merge_speakers(transcript, diarized)

    assert [segment.speaker for segment in merged.segments] == [
        "SPEAKER_00",
        "SPEAKER_01",
    ]


class _SegmentTranscriber:
    name = "segments"

    async def transcribe(self, audio: AudioRef, opts: object) -> Transcript:
        return Transcript(
            text="hello world",
            segments=[
                TranscriptSegment(text="hello", start=0.0, end=1.0),
                TranscriptSegment(text="world", start=1.0, end=2.0),
            ],
        )


class _SpeakerDiarizer:
    name = "speakers"

    async def diarize(self, audio: AudioRef) -> object:
        return {
            "segments": [
                {"start": 0.0, "end": 1.0, "speaker": "SPEAKER_00"},
                {"start": 1.0, "end": 2.0, "speaker": "SPEAKER_01"},
            ]
        }


class _NoopSummarizer:
    name = "none"

    async def summarize(
        self, transcript: Transcript, *, instructions: str | None = None
    ) -> Summary:
        raise AssertionError("summarizer should not run in this test")


async def test_pipeline_merges_speaker_labels_into_transcript_segments(
    tmp_path: Path,
) -> None:
    pipeline = object.__new__(Pipeline)
    pipeline.transcriber = _SegmentTranscriber()
    pipeline.diarizer = _SpeakerDiarizer()
    pipeline.summarizer = _NoopSummarizer()

    rec = Recording(id="rec-1", audio=_audio(tmp_path))
    result = await pipeline.run(rec, diarize=True, summarize=False)

    assert result.status is JobStatus.DONE
    assert result.transcript is not None
    assert [segment.speaker for segment in result.transcript.segments] == [
        "SPEAKER_00",
        "SPEAKER_01",
    ]


class _Turn:
    def __init__(self, start: float, end: float) -> None:
        self.start = start
        self.end = end


class _Annotation:
    def itertracks(self, *, yield_label: bool) -> list[tuple[_Turn, object, str]]:
        assert yield_label is True
        return [(_Turn(0.0, 1.25), object(), "SPEAKER_00")]


async def test_pyannote_provider_uses_dynamic_pipeline() -> None:
    calls: list[tuple[str, str]] = []

    def pipeline(path: str) -> _Annotation:
        calls.append(("call", path))
        return _Annotation()

    provider = PyannoteDiarizer(pipeline=pipeline)
    result = await provider.diarize(
        AudioRef(
            path=Path("/tmp/audio.wav"),
            filename="audio.wav",
            content_type=None,
            size_bytes=1,
        )
    )

    assert calls == [("call", "/tmp/audio.wav")]
    assert result == {"segments": [{"start": 0.0, "end": 1.25, "speaker": "SPEAKER_00"}]}


def test_pyannote_provider_loads_module(monkeypatch: pytest.MonkeyPatch) -> None:
    class _PipelineFactory:
        @staticmethod
        def from_pretrained(model: str, use_auth_token: str | None = None) -> object:
            return lambda path: _Annotation()

    module = types.SimpleNamespace(Pipeline=_PipelineFactory)
    monkeypatch.setitem(sys.modules, "pyannote.audio", module)

    provider = PyannoteDiarizer(model="local-model", auth_token="token")

    assert provider._load_pipeline() is provider._load_pipeline()


def _fake_whisperx_cli(tmp_path: Path, *, exit_code: int = 0) -> Path:
    script = tmp_path / "fake-whisperx"
    script.write_text(
        "\n".join(
            [
                "#!/usr/bin/env python3",
                "from pathlib import Path",
                "import sys",
                f"EXIT_CODE = {exit_code}",
                "if EXIT_CODE:",
                "    print('forced failure', file=sys.stderr)",
                "    raise SystemExit(EXIT_CODE)",
                "out = Path(sys.argv[sys.argv.index('--output_dir') + 1]) / 'rec.json'",
                "out.write_text('{\"segments\":[{\"start\":0,\"end\":1.5,\"speaker\":\"SPEAKER_00\",\"text\":\"hi\"}]}', encoding='utf-8')",  # noqa: E501
            ]
        ),
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


async def test_whisperx_provider_runs_cli_and_normalizes_json(tmp_path: Path) -> None:
    provider = WhisperXDiarizer(
        binary=str(_fake_whisperx_cli(tmp_path)),
        model="base",
        timeout_seconds=5,
    )

    assert await provider.diarize(_audio(tmp_path)) == {
        "segments": [
            {"start": 0.0, "end": 1.5, "speaker": "SPEAKER_00", "text": "hi"}
        ]
    }


async def test_whisperx_provider_nonzero_exit_raises(tmp_path: Path) -> None:
    provider = WhisperXDiarizer(
        binary=str(_fake_whisperx_cli(tmp_path, exit_code=9)),
        timeout_seconds=5,
    )

    with pytest.raises(RuntimeError, match="exit code 9"):
        await provider.diarize(_audio(tmp_path))


def test_package_wiring_exposes_diarization_providers() -> None:
    assert {"none", "pyannote", "whisperx"} <= set(diarization.available())
