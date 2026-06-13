"""Durable SQLite store for recordings.

Two design decisions dominate this module:

1. **Lazy, resettable connection.** We never read ``config.storage_dir`` or open a
   connection in ``__init__``. The path is resolved (and the directory created) on
   the *first* operation, from the live ``self._config`` object. Tests reassign
   ``config.storage_dir = tmp_path`` after the store is constructed and reset state
   per test; binding the path early would leak one test's DB into the next and
   ignore ``tmp_path`` entirely. ``reset()`` drops the cached connection so the next
   call reconnects against whatever ``storage_dir`` now points at.

2. **Concurrency.** The pipeline runs in a FastAPI ``BackgroundTask`` — an async
   task on the event loop — so its ``update()`` interleaves with a GET's ``get()``.
   Store methods hold no ``await``, so each is already atomic w.r.t. other
   coroutines; we still open with ``check_same_thread=False`` and serialise every
   public method under one non-reentrant ``threading.Lock`` (plus WAL and a
   ``busy_timeout``) so a future threaded or multi-worker caller stays safe.
"""
from __future__ import annotations

import json
import sqlite3
import threading
from datetime import datetime
from pathlib import Path

from open_plaud.config import Config
from open_plaud.types import (
    ActionItem,
    AudioRef,
    JobStatus,
    Recording,
    Summary,
    Transcript,
    TranscriptSegment,
)

_CREATE_TABLE = """
CREATE TABLE IF NOT EXISTS recordings (
    id TEXT PRIMARY KEY,
    status TEXT,
    created_at TEXT,
    audio_path TEXT,
    audio_filename TEXT,
    audio_content_type TEXT,
    audio_size_bytes INTEGER,
    transcript_text TEXT,
    transcript_language TEXT,
    transcript_segments_json TEXT,
    diarization_json TEXT,
    summary_json TEXT,
    error TEXT
)
"""

_UPSERT = """
INSERT OR REPLACE INTO recordings (
    id, status, created_at,
    audio_path, audio_filename, audio_content_type, audio_size_bytes,
    transcript_text, transcript_language, transcript_segments_json,
    diarization_json, summary_json, error
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
"""

_COLUMNS = (
    "id, status, created_at, "
    "audio_path, audio_filename, audio_content_type, audio_size_bytes, "
    "transcript_text, transcript_language, transcript_segments_json, "
    "diarization_json, summary_json, error"
)


class RecordingStore:
    """SQLite-backed persistence for :class:`Recording` objects."""

    def __init__(self, config: Config) -> None:
        # Hold the SHARED config object by reference only. Reading storage_dir or
        # opening a connection here would defeat the lazy-path contract above.
        self._config = config
        self._conn: sqlite3.Connection | None = None
        self._lock = threading.Lock()

    # -- connection management -------------------------------------------------

    def _connect(self) -> sqlite3.Connection:
        """Return the cached connection, opening one on first use.

        Assumes ``self._lock`` is already held by the caller — the lock is
        non-reentrant, so this helper must never re-acquire it.
        """
        if self._conn is None:
            # Resolve the path NOW, from the live config, so a post-construction
            # reassignment of storage_dir (and tmp_path in tests) is respected.
            db_path = self._config.storage_dir / "recordings.db"
            db_path.parent.mkdir(parents=True, exist_ok=True)
            conn = sqlite3.connect(str(db_path), check_same_thread=False)
            # WAL lets a reader (GET) and a writer (background pipeline) proceed
            # without blocking each other on the shared connection.
            conn.execute("PRAGMA journal_mode=WAL")
            # Degrade to a short wait (not an immediate SQLITE_BUSY) if a future
            # multi-worker deploy has another process mid-write.
            conn.execute("PRAGMA busy_timeout=5000")
            conn.execute(_CREATE_TABLE)
            _migrate(conn)
            conn.commit()
            self._conn = conn
        return self._conn

    def reset(self) -> None:
        """Close and clear the cached connection.

        The next operation reconnects against the current ``storage_dir``. Tests
        rely on this to point a fresh store at ``tmp_path`` between cases.
        """
        with self._lock:
            if self._conn is not None:
                self._conn.close()
                self._conn = None

    # -- writes ----------------------------------------------------------------

    def add(self, rec: Recording) -> None:
        with self._lock:
            conn = self._connect()
            conn.execute(_UPSERT, _to_row(rec))
            # Commit on every write: sqlite rolls back uncommitted work on close(),
            # so without this the durability-across-restart contract breaks.
            conn.commit()

    def update(self, rec: Recording) -> None:
        # Same upsert as add(): INSERT OR REPLACE is idempotent on the primary key.
        with self._lock:
            conn = self._connect()
            conn.execute(_UPSERT, _to_row(rec))
            conn.commit()

    # -- reads -----------------------------------------------------------------

    def get(self, recording_id: str) -> Recording | None:
        with self._lock:
            conn = self._connect()
            cur = conn.execute(
                f"SELECT {_COLUMNS} FROM recordings WHERE id = ?",
                (recording_id,),
            )
            row = cur.fetchone()
        return _from_row(row) if row is not None else None

    def list(self) -> list[Recording]:
        with self._lock:
            conn = self._connect()
            cur = conn.execute(
                f"SELECT {_COLUMNS} FROM recordings ORDER BY created_at DESC"
            )
            rows = cur.fetchall()
        return [_from_row(row) for row in rows]


# -- (de)serialization helpers -------------------------------------------------


def _as_int(value: object) -> int:
    """Narrow an ``object`` row value to ``int`` for mypy --strict.

    SQLite hands back the INTEGER column as a Python ``int``; the row tuple is
    typed ``object``, so we assert rather than blind-cast to fail loud on a
    corrupt/unexpected value.
    """
    assert isinstance(value, int)
    return value


def _migrate(conn: sqlite3.Connection) -> None:
    columns = {row[1] for row in conn.execute("PRAGMA table_info(recordings)")}
    if "transcript_segments_json" not in columns:
        conn.execute("ALTER TABLE recordings ADD COLUMN transcript_segments_json TEXT")


def _to_row(rec: Recording) -> tuple[object, ...]:
    """Flatten a Recording into a parameter tuple for the upsert."""
    transcript_text = rec.transcript.text if rec.transcript is not None else None
    transcript_language = (
        rec.transcript.language if rec.transcript is not None else None
    )
    transcript_segments_json = (
        json.dumps(
            [
                {
                    "text": segment.text,
                    "start": segment.start,
                    "end": segment.end,
                    "speaker": segment.speaker,
                }
                for segment in rec.transcript.segments
            ]
        )
        if rec.transcript is not None and rec.transcript.segments
        else None
    )

    diarization_json = (
        json.dumps(rec.diarization) if rec.diarization is not None else None
    )

    if rec.summary is not None:
        # Per the store contract, summary is a Summary dataclass. Narrow with an
        # assert (over cast) so a contract violation fails loudly rather than
        # silently corrupting the row.
        assert isinstance(rec.summary, Summary)
        summary_json = json.dumps(
            {
                "summary": rec.summary.summary,
                "action_items": [
                    {"text": item.text, "owner": item.owner, "due": item.due}
                    for item in rec.summary.action_items
                ],
            }
        )
    else:
        summary_json = None

    return (
        rec.id,
        rec.status.value,
        rec.created_at.isoformat(),
        str(rec.audio.path),
        rec.audio.filename,
        rec.audio.content_type,
        rec.audio.size_bytes,
        transcript_text,
        transcript_language,
        transcript_segments_json,
        diarization_json,
        summary_json,
        rec.error,
    )


def _from_row(row: tuple[object, ...]) -> Recording:
    """Rehydrate a full Recording from a database row (column order: _COLUMNS)."""
    (
        id_,
        status,
        created_at,
        audio_path,
        audio_filename,
        audio_content_type,
        audio_size_bytes,
        transcript_text,
        transcript_language,
        transcript_segments_json,
        diarization_json,
        summary_json,
        error,
    ) = row

    transcript: Transcript | None = None
    if transcript_text is not None:
        segments: list[TranscriptSegment] = []
        if transcript_segments_json is not None:
            raw_segments = json.loads(str(transcript_segments_json))
            assert isinstance(raw_segments, list)
            segments = [
                TranscriptSegment(
                    text=str(item["text"]),
                    start=(
                        float(item["start"]) if item.get("start") is not None else None
                    ),
                    end=float(item["end"]) if item.get("end") is not None else None,
                    speaker=(
                        str(item["speaker"]) if item.get("speaker") is not None else None
                    ),
                )
                for item in raw_segments
            ]
        transcript = Transcript(
            text=str(transcript_text),
            language=(
                str(transcript_language) if transcript_language is not None else None
            ),
            segments=segments,
        )

    diarization: object | None = None
    if diarization_json is not None:
        diarization = json.loads(str(diarization_json))

    summary: Summary | None = None
    if summary_json is not None:
        raw = json.loads(str(summary_json))
        summary = Summary(
            summary=raw["summary"],
            action_items=[
                ActionItem(text=item["text"], owner=item["owner"], due=item["due"])
                for item in raw["action_items"]
            ],
        )

    return Recording(
        id=str(id_),
        audio=AudioRef(
            path=Path(str(audio_path)),
            filename=str(audio_filename),
            content_type=(
                str(audio_content_type) if audio_content_type is not None else None
            ),
            size_bytes=_as_int(audio_size_bytes),
        ),
        status=JobStatus(str(status)),
        created_at=datetime.fromisoformat(str(created_at)),
        transcript=transcript,
        diarization=diarization,
        summary=summary,
        error=str(error) if error is not None else None,
    )
