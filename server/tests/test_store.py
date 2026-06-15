from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

from open_plaud.config import Config
from open_plaud.store import RecordingStore
from open_plaud.types import (
    ActionItem,
    AudioRef,
    JobStatus,
    Recording,
    Summary,
    Transcript,
    TranscriptSegment,
)


def _store(tmp_path: Path) -> RecordingStore:
    # pydantic-settings accepts field kwargs directly.
    return RecordingStore(Config(storage_dir=tmp_path))


def _full_recording(rec_id: str = "rec-1") -> Recording:
    """A fully-populated Recording exercising every column."""
    return Recording(
        id=rec_id,
        audio=AudioRef(
            path=Path("/data/audio/rec-1.wav"),
            filename="rec-1.wav",
            content_type="audio/wav",
            size_bytes=2048,
            duration_seconds=12.5,
        ),
        status=JobStatus.DONE,
        created_at=datetime(2026, 1, 2, 3, 4, 5, tzinfo=timezone.utc),
        transcript=Transcript(
            text="hello world",
            language="en",
            segments=[
                TranscriptSegment(
                    text="hello world",
                    start=0.0,
                    end=1.5,
                    speaker="SPEAKER_00",
                )
            ],
        ),
        # JSON-native values only: a tuple would round-trip as a list and break ==.
        diarization={"segments": [{"speaker": "A", "start": 0.0, "end": 1.5}]},
        summary=Summary(
            summary="A short meeting.",
            action_items=[
                ActionItem(text="ship the store", owner="guzus", due="2026-06-09"),
                # owner/due left as None to prove NULL-in-JSON round-trips.
                ActionItem(text="write tests"),
            ],
        ),
        error=None,
    )


def test_add_get_round_trip(tmp_path: Path) -> None:
    store = _store(tmp_path)
    rec = _full_recording()
    store.add(rec)

    loaded = store.get(rec.id)
    assert loaded is not None
    # Deep, field-by-field equality: every nested type is a dataclass with a
    # generated __eq__, and the tz-aware datetime round-trips exactly.
    assert loaded == rec

    # Spell out the load-bearing nested fields for failure readability.
    assert loaded.status is JobStatus.DONE
    assert loaded.audio.path == Path("/data/audio/rec-1.wav")
    assert loaded.audio.content_type == "audio/wav"
    assert loaded.audio.duration_seconds == 12.5
    assert loaded.transcript == Transcript(
        text="hello world",
        language="en",
        segments=[
            TranscriptSegment(
                text="hello world",
                start=0.0,
                end=1.5,
                speaker="SPEAKER_00",
            )
        ],
    )
    assert loaded.diarization == {"segments": [{"speaker": "A", "start": 0.0, "end": 1.5}]}
    assert isinstance(loaded.summary, Summary)
    assert loaded.summary.action_items[1] == ActionItem(text="write tests")
    assert loaded.summary.action_items[1].owner is None


def test_update_mutates_existing_row(tmp_path: Path) -> None:
    store = _store(tmp_path)
    rec = Recording(
        id="rec-2",
        audio=AudioRef(
            path=Path("/data/audio/rec-2.wav"),
            filename="rec-2.wav",
            content_type=None,
            size_bytes=10,
        ),
        status=JobStatus.RECEIVED,
        created_at=datetime(2026, 1, 1, tzinfo=timezone.utc),
    )
    store.add(rec)

    # Mutate the same id: advance status and attach transcript + summary.
    rec.status = JobStatus.DONE
    rec.transcript = Transcript(text="done now", language=None)
    rec.summary = Summary(
        summary="wrapped up",
        action_items=[ActionItem(text="follow up", owner="alice")],
    )
    store.update(rec)

    loaded = store.get("rec-2")
    assert loaded is not None
    assert loaded.status is JobStatus.DONE
    assert loaded.transcript == Transcript(text="done now", language=None)
    assert isinstance(loaded.summary, Summary)
    assert loaded.summary.summary == "wrapped up"
    assert loaded.summary.action_items == [ActionItem(text="follow up", owner="alice")]
    # content_type was None at add time and must survive as None.
    assert loaded.audio.content_type is None
    # Exactly one row — update upserted rather than inserted a duplicate.
    assert len(store.list()) == 1


def test_get_missing_returns_none(tmp_path: Path) -> None:
    store = _store(tmp_path)
    assert store.get("nope") is None


def test_list_orders_newest_first(tmp_path: Path) -> None:
    store = _store(tmp_path)

    def _rec(rec_id: str, when: datetime) -> Recording:
        return Recording(
            id=rec_id,
            audio=AudioRef(
                path=Path(f"/data/{rec_id}.wav"),
                filename=f"{rec_id}.wav",
                content_type="audio/wav",
                size_bytes=1,
            ),
            created_at=when,
        )

    # Distinct UTC timestamps (shared +00:00 offset) so the TEXT ORDER BY is
    # deterministically chronological.
    old = _rec("old", datetime(2026, 1, 1, tzinfo=timezone.utc))
    mid = _rec("mid", datetime(2026, 3, 1, tzinfo=timezone.utc))
    new = _rec("new", datetime(2026, 6, 1, tzinfo=timezone.utc))
    # Insert out of order to prove ordering comes from the query, not insertion.
    store.add(mid)
    store.add(old)
    store.add(new)

    assert [r.id for r in store.list()] == ["new", "mid", "old"]


def test_durability_across_restart(tmp_path: Path) -> None:
    # Store A writes; store B opens the SAME path. After A closes its connection,
    # B must still read the record — proving it landed on disk, not just memory.
    config = Config(storage_dir=tmp_path)
    a = RecordingStore(config)
    b = RecordingStore(config)

    rec = _full_recording("persist-me")
    a.add(rec)
    a.reset()  # close A's connection (rolls back anything uncommitted)

    loaded = b.get("persist-me")
    assert loaded is not None
    assert loaded == rec
