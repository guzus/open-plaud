"""Find and repair recordings stored as raw ADPCM under a `.wav` name.

**Why this exists.** Before ADPCM ingest landed, the upload endpoint decided
whether an upload was audio by looking at the filename suffix or the content
type, and `rec_0001.wav` passes a suffix check. A stock device sends exactly
that name with `application/vnd.openplaud.adpcm` bytes behind it (see
`audio_ingest`), so the server answered **HTTP 200** and wrote the raw transfer
container to disk under a `.wav` name. Nothing failed loudly: the file is
unplayable, `whisper_cpp` cannot read it, and the pipeline had already run over
it and stored whatever garbage came back. Anyone who synced a device before
upgrading has those files on disk right now, and upgrading does not repair them.

So this is a one-shot migration for existing installs:

    python -m open_plaud.repair              # report only (default)
    python -m open_plaud.repair --apply      # decode in place and reset rows

**Dry-run is the default on purpose** — it touches a self-hoster's audio and
their SQLite rows, so it never runs implicitly at import or on server startup.

Repaired rows are reset to `received` with transcript, diarization, summary and
error cleared. Fixing the audio while leaving the old transcript in place would
swap one silent corruption for another: that text was produced from garbage
input and is not a transcript of the clip. `received` is the honest state — the
audio is good and no valid transcript exists yet — and it is the state the web
UI's reprocess action expects. Re-run the pipeline afterwards with
``POST /rec/{id}/reprocess`` (or the "Reprocess" button) for each id reported.

Idempotent: a repaired file no longer starts with the container magic, so a
second run reports nothing to do.
"""

from __future__ import annotations

import argparse
import wave
from dataclasses import dataclass
from pathlib import Path

from open_plaud.audio_ingest import (
    OpenPlaudADPCMError,
    looks_like_opadpcm,
    unwrap_opadpcm_file,
)
from open_plaud.config import Config
from open_plaud.store import RecordingStore
from open_plaud.types import JobStatus, Recording


@dataclass(slots=True)
class RepairOutcome:
    """One recording's result. `error` set means the row was left untouched."""

    recording_id: str
    path: Path
    repaired: bool
    before_bytes: int
    after_bytes: int = 0
    error: str | None = None


def _wav_duration_seconds(path: Path) -> float | None:
    """Same rule as the API's duration probe: WAV only, failures are `None`."""
    if path.suffix.lower() != ".wav":
        return None
    try:
        with wave.open(str(path), "rb") as wav:
            frame_rate = wav.getframerate()
            if frame_rate <= 0:
                return None
            return wav.getnframes() / frame_rate
    except (EOFError, wave.Error, OSError):
        return None


def find_corrupt(store: RecordingStore) -> list[Recording]:
    """Recordings whose stored blob is still an undecoded ADPCM container.

    Detection is by magic bytes, never by suffix or by the recorded content type
    — the whole reason these rows exist is that the name lied about the bytes.
    """
    return [rec for rec in store.list() if looks_like_opadpcm(rec.audio.path)]


def repair_recording(
    store: RecordingStore, rec: Recording, *, max_output_bytes: int
) -> RepairOutcome:
    """Decode one recording in place and reset it for reprocessing.

    The blob is rewritten via the same temp-file+rename the upload path uses, so
    a crash mid-repair leaves either the original container or the finished WAV
    on disk — never a truncated file. The row is only updated after the rename
    succeeds, so the DB never points at audio that is not there.
    """
    before_bytes = rec.audio.size_bytes
    try:
        final, filename, content_type = unwrap_opadpcm_file(
            rec.audio.path, rec.audio.filename, max_output_bytes=max_output_bytes
        )
    except (OpenPlaudADPCMError, OSError) as exc:
        return RepairOutcome(
            recording_id=rec.id,
            path=rec.audio.path,
            repaired=False,
            before_bytes=before_bytes,
            error=str(exc),
        )

    rec.audio.path = final
    rec.audio.filename = filename
    rec.audio.content_type = content_type
    rec.audio.size_bytes = final.stat().st_size
    rec.audio.duration_seconds = _wav_duration_seconds(final)

    # The old results were derived from the corrupt bytes. Keeping them would
    # leave the install looking healthy while still serving nonsense.
    rec.status = JobStatus.RECEIVED
    rec.transcript = None
    rec.diarization = None
    rec.summary = None
    rec.error = None
    store.update(rec)

    return RepairOutcome(
        recording_id=rec.id,
        path=final,
        repaired=True,
        before_bytes=before_bytes,
        after_bytes=rec.audio.size_bytes,
    )


def run(store: RecordingStore, *, apply: bool, max_output_bytes: int) -> list[RepairOutcome]:
    """Scan, and repair only when `apply` is true. Returns one result per hit."""
    corrupt = find_corrupt(store)
    if not apply:
        return [
            RepairOutcome(
                recording_id=rec.id,
                path=rec.audio.path,
                repaired=False,
                before_bytes=rec.audio.size_bytes,
            )
            for rec in corrupt
        ]
    return [
        repair_recording(store, rec, max_output_bytes=max_output_bytes) for rec in corrupt
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m open_plaud.repair",
        description=(
            "Repair recordings stored as raw Open Plaud ADPCM under a .wav name "
            "(uploads accepted by servers predating ADPCM ingest)."
        ),
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="decode the files in place and reset the rows (default: report only)",
    )
    args = parser.parse_args(argv)

    config = Config()
    store = RecordingStore(config)
    results = run(store, apply=args.apply, max_output_bytes=config.upload_max_bytes)

    if not results:
        print("No corrupt Open Plaud ADPCM recordings found.")
        return 0

    failures = [item for item in results if item.error]
    if not args.apply:
        print(f"Found {len(results)} recording(s) stored as raw ADPCM (dry run):")
        for item in results:
            print(f"  {item.recording_id}  {item.path}  {item.before_bytes} bytes")
        print("\nRe-run with --apply to decode them to PCM WAV.")
        return 0

    repaired = [item for item in results if item.repaired]
    print(f"Repaired {len(repaired)} of {len(results)} recording(s):")
    for item in repaired:
        print(
            f"  {item.recording_id}  {item.path}  "
            f"{item.before_bytes} -> {item.after_bytes} bytes"
        )
    for item in failures:
        print(f"  FAILED {item.recording_id}  {item.path}: {item.error}")

    if repaired:
        print(
            "\nThese recordings were reset to 'received' and hold no transcript: "
            "the old one was produced from corrupt audio. Re-run the pipeline for "
            "each with POST /rec/{id}/reprocess."
        )
    return 1 if failures else 0


if __name__ == "__main__":  # pragma: no cover - CLI entry point
    raise SystemExit(main())
