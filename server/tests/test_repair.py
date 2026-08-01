"""Repair of recordings a pre-ADPCM server stored as raw containers.

The fixture here reproduces what the old server actually left on disk — proven
against origin/main, which answered HTTP 200 and wrote the container verbatim to
`<id>.wav` — rather than assuming a shape.
"""

from __future__ import annotations

import struct
import wave
from collections.abc import Iterator
from datetime import datetime, timezone
from pathlib import Path

import opadpcm_fixtures
import pytest

from open_plaud.config import Config
from open_plaud.repair import find_corrupt, main, repair_recording, run
from open_plaud.store import RecordingStore
from open_plaud.types import AudioRef, JobStatus, Recording, Summary, Transcript


@pytest.fixture
def store(tmp_path: Path) -> Iterator[RecordingStore]:
    store = RecordingStore(Config(storage_dir=tmp_path))
    yield store
    store.reset()


def _audio_dir(store: RecordingStore) -> Path:
    """Where the API writes blobs: `storage_dir/audio` (see app._storage_dir)."""
    path = store._config.storage_dir / "audio"  # noqa: SLF001 - fixture setup
    path.mkdir(parents=True, exist_ok=True)
    return path


def _store_as_old_server_did(
    store: RecordingStore, recording_id: str, samples: int = 4000
) -> tuple[Path, list[int]]:
    """Write the container to `<id>.wav` and mark the row done, as main did.

    main ran the pipeline over the corrupt bytes, so the row carries a transcript
    and a summary that describe nothing.
    """
    raw, expected = opadpcm_fixtures.device_container(samples=samples)
    path = _audio_dir(store) / f"{recording_id}.wav"
    path.write_bytes(raw)

    store.add(
        Recording(
            id=recording_id,
            audio=AudioRef(
                path=path,
                filename="rec_0001.wav",
                content_type="application/vnd.openplaud.adpcm",
                size_bytes=len(raw),
                duration_seconds=None,
            ),
            status=JobStatus.DONE,
            created_at=datetime.now(timezone.utc),
            transcript=Transcript(text="garbage from corrupt audio", language="en"),
            summary=Summary(summary="nonsense summary", action_items=[]),
        )
    )
    return path, expected


def test_find_corrupt_detects_container_under_a_wav_name(store: RecordingStore) -> None:
    _store_as_old_server_did(store, "corrupt1")
    healthy = _audio_dir(store) / "healthy1.wav"
    healthy.write_bytes(b"RIFF____WAVEfmt ")
    store.add(
        Recording(
            id="healthy1",
            audio=AudioRef(
                path=healthy,
                filename="rec_0002.wav",
                content_type="audio/wav",
                size_bytes=healthy.stat().st_size,
            ),
        )
    )

    assert [rec.id for rec in find_corrupt(store)] == ["corrupt1"]


def test_repair_decodes_in_place_and_clears_stale_results(store: RecordingStore) -> None:
    path, expected = _store_as_old_server_did(store, "corrupt1")
    original_container = path.read_bytes()

    stale = store.get("corrupt1")
    assert stale is not None
    outcome = repair_recording(store, stale, max_output_bytes=512 * 1024 * 1024)

    assert outcome.repaired
    assert outcome.before_bytes == len(original_container)
    assert outcome.after_bytes == 44 + 4000 * 2

    # Same path — the corrupt blob was already sitting at the .wav name.
    assert outcome.path == path
    assert path.stat().st_size == 44 + 4000 * 2
    assert not list(path.parent.glob("*.tmp"))

    with wave.open(str(path), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 16000
        assert wav.getnframes() == 4000
        frames = wav.readframes(wav.getnframes())
    assert list(struct.unpack(f"<{len(frames) // 2}h", frames)) == expected

    row = store.get("corrupt1")
    assert row is not None
    assert row.audio.content_type == "audio/wav"
    assert row.audio.size_bytes == 44 + 4000 * 2
    assert row.audio.duration_seconds == pytest.approx(0.25)
    # The old transcript described corrupt bytes; keeping it would hide the damage.
    assert row.status is JobStatus.RECEIVED
    assert row.transcript is None
    assert row.summary is None
    assert row.error is None


def test_dry_run_is_the_default_and_changes_nothing(store: RecordingStore) -> None:
    path, _ = _store_as_old_server_did(store, "corrupt1")
    before = path.read_bytes()

    results = run(store, apply=False, max_output_bytes=512 * 1024 * 1024)

    assert [item.recording_id for item in results] == ["corrupt1"]
    assert not any(item.repaired for item in results)
    assert path.read_bytes() == before
    row = store.get("corrupt1")
    assert row is not None
    assert row.status is JobStatus.DONE


def test_repair_is_idempotent(store: RecordingStore) -> None:
    _store_as_old_server_did(store, "corrupt1")

    first = run(store, apply=True, max_output_bytes=512 * 1024 * 1024)
    second = run(store, apply=True, max_output_bytes=512 * 1024 * 1024)

    assert len(first) == 1 and first[0].repaired
    assert second == []


def test_truncated_container_is_reported_and_left_alone(store: RecordingStore) -> None:
    path, _ = _store_as_old_server_did(store, "corrupt1")
    path.write_bytes(path.read_bytes()[:-500])
    damaged = path.read_bytes()

    results = run(store, apply=True, max_output_bytes=512 * 1024 * 1024)

    assert len(results) == 1
    assert not results[0].repaired
    assert results[0].error is not None
    # A file we cannot decode must survive untouched: the operator may still be
    # able to recover it by hand, and a rewrite would destroy that chance.
    assert path.read_bytes() == damaged
    assert not list(path.parent.glob("*.tmp"))


def test_output_cap_rejects_without_destroying_the_source(store: RecordingStore) -> None:
    path, _ = _store_as_old_server_did(store, "corrupt1")
    original = path.read_bytes()

    results = run(store, apply=True, max_output_bytes=1024)

    assert not results[0].repaired
    assert "exceed" in str(results[0].error)
    assert path.read_bytes() == original


def test_cli_dry_run_reports_and_apply_repairs(
    store: RecordingStore, tmp_path: Path, capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _store_as_old_server_did(store, "corrupt1")
    store.reset()
    monkeypatch.setenv("OP_STORAGE_DIR", str(tmp_path))

    assert main([]) == 0
    dry = capsys.readouterr().out
    assert "corrupt1" in dry
    assert "--apply" in dry

    assert main(["--apply"]) == 0
    applied = capsys.readouterr().out
    assert "Repaired 1 of 1" in applied
    assert "reprocess" in applied

    assert main([]) == 0
    assert "No corrupt" in capsys.readouterr().out
