"""open-plaud HTTP API.

The device uploads audio to ``POST /v1/recordings``; the server stores the blob,
persists a row in the SQLite store, and kicks off the transcribe -> summarize
pipeline as a background task so the upload returns immediately. The device (or
the bundled web UI) then polls ``GET /v1/recordings/{id}`` for progress and the
final transcript + summary.

State lives in two module-level singletons resolved from config:
  - ``store``    durable SQLite persistence (survives restart)
  - ``pipeline`` the provider-agnostic orchestrator (none/none by default)
Both are constructed once at import; with the default ``none`` providers nothing
needs an API key, so import is always safe and tests stay offline.
"""

from __future__ import annotations

from collections.abc import Iterable
from datetime import datetime
import logging
from pathlib import Path
import wave
from uuid import uuid4

from fastapi import BackgroundTasks, FastAPI, File, HTTPException, Request, UploadFile
from fastapi.responses import (
    FileResponse,
    HTMLResponse,
    PlainTextResponse,
    RedirectResponse,
)
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel, ConfigDict, Field

from open_plaud.config import Config
from open_plaud.hooker import HookerDispatcher
from open_plaud.pipeline.orchestrator import Pipeline
from open_plaud.store import RecordingStore
from open_plaud.types import AudioRef, JobStatus, Recording, Summary

_AUDIO_SUFFIXES = {".wav", ".flac", ".m4a", ".mp3"}
_AUDIO_MEDIA_TYPES = {"audio/wav", "audio/x-wav", "audio/flac", "audio/mpeg", "audio/mp4"}
_SUFFIX_MEDIA_TYPES = {
    ".wav": "audio/wav",
    ".flac": "audio/flac",
    ".m4a": "audio/mp4",
    ".mp3": "audio/mpeg",
}
log = logging.getLogger("open_plaud.api")


class TranscriptSegmentResponse(BaseModel):
    text: str
    start: float | None = None
    end: float | None = None
    speaker: str | None = None


class TranscriptResponse(BaseModel):
    text: str
    language: str | None = None
    segments: list[TranscriptSegmentResponse] = Field(default_factory=list)


class ActionItemResponse(BaseModel):
    text: str
    owner: str | None = None
    due: str | None = None


class SummaryResponse(BaseModel):
    summary: str
    action_items: list[ActionItemResponse]


class RecordingResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: str
    status: JobStatus
    filename: str
    size_bytes: int
    duration_seconds: float | None = None
    created_at: datetime
    transcript: TranscriptResponse | None = None
    summary: SummaryResponse | None = None
    error: str | None = None


class HealthResponse(BaseModel):
    status: str


app = FastAPI(title="open-plaud API", version="0.0.1")
config = Config()
store = RecordingStore(config)
pipeline = Pipeline(config)
hooker_dispatcher = HookerDispatcher(config.hooker)
templates = Jinja2Templates(directory=str(Path(__file__).parent / "templates"))


def _storage_dir() -> Path:
    path = config.storage_dir / "audio"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _safe_suffix(filename: str | None) -> str:
    suffix = Path(filename or "").suffix.lower()
    return suffix if suffix in _AUDIO_SUFFIXES else ".wav"


def _is_supported_audio_upload(filename: str | None, content_type: str | None) -> bool:
    suffix = Path(filename or "").suffix.lower()
    return suffix in _AUDIO_SUFFIXES or content_type in _AUDIO_MEDIA_TYPES


def _audio_media_type(rec: Recording) -> str:
    # Never reflect the client-supplied content_type unchecked — that would let an
    # upload serve e.g. text/html from our origin. Allowlist it, else derive from
    # the server-controlled suffix, else fall back to an opaque type.
    if rec.audio.content_type in _AUDIO_MEDIA_TYPES:
        return rec.audio.content_type
    return _SUFFIX_MEDIA_TYPES.get(rec.audio.path.suffix.lower(), "application/octet-stream")


def _audio_duration_seconds(path: Path) -> float | None:
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


def _recording_duration_seconds(rec: Recording) -> float | None:
    return rec.audio.duration_seconds or _audio_duration_seconds(rec.audio.path)


def _format_duration(seconds: float | None) -> str:
    if seconds is None:
        return "unknown"
    total_seconds = max(0, round(seconds))
    minutes, secs = divmod(total_seconds, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{secs:02d}"
    return f"{minutes}:{secs:02d}"


def _format_bytes(size_bytes: int) -> str:
    units = ("B", "KB", "MB", "GB")
    value = float(size_bytes)
    for unit in units:
        if value < 1024 or unit == units[-1]:
            if unit == "B":
                return f"{int(value)} B"
            return f"{value:.1f} {unit}"
        value /= 1024
    return f"{size_bytes} B"


def _to_response(rec: Recording) -> RecordingResponse:
    transcript = (
        TranscriptResponse(
            text=rec.transcript.text,
            language=rec.transcript.language,
            segments=[
                TranscriptSegmentResponse(
                    text=segment.text,
                    start=segment.start,
                    end=segment.end,
                    speaker=segment.speaker,
                )
                for segment in rec.transcript.segments
            ],
        )
        if rec.transcript is not None
        else None
    )
    summary: SummaryResponse | None = None
    if isinstance(rec.summary, Summary):
        summary = SummaryResponse(
            summary=rec.summary.summary,
            action_items=[
                ActionItemResponse(text=item.text, owner=item.owner, due=item.due)
                for item in rec.summary.action_items
            ],
        )
    return RecordingResponse(
        id=rec.id,
        status=rec.status,
        filename=rec.audio.filename,
        size_bytes=rec.audio.size_bytes,
        duration_seconds=_recording_duration_seconds(rec),
        created_at=rec.created_at,
        transcript=transcript,
        summary=summary,
        error=rec.error,
    )


@app.get("/healthz", response_model=HealthResponse)
async def healthz() -> HealthResponse:
    return HealthResponse(status="ok")


async def _process(recording_id: str) -> None:
    """Run the pipeline for one recording and persist the result.

    Scheduled as a background task so the upload responds immediately. We persist
    ``TRANSCRIBING`` up front so a poller sees that work has started, let the
    orchestrator drive the remaining status transitions in-memory, then persist
    the terminal state (``done`` or ``failed``). The orchestrator never touches
    the store — it stays a pure, persistence-agnostic stage runner.
    """
    rec = store.get(recording_id)
    if rec is None:
        return
    rec.status = JobStatus.TRANSCRIBING
    store.update(rec)
    await pipeline.run(rec, summarize=True)
    if rec.status == JobStatus.DONE:
        try:
            result = await hooker_dispatcher.dispatch(rec)
            if result.published:
                log.info("dispatched recording %s to Hooker topic %s", rec.id, config.hooker.topic)
            elif result.reason != "disabled":
                log.info("skipped Hooker dispatch for recording %s: %s", rec.id, result.reason)
        except Exception as exc:  # noqa: BLE001 - keep audio/transcript usable
            log.exception("Hooker dispatch failed for recording %s", rec.id)
            rec.error = f"Hooker dispatch failed: {type(exc).__name__}: {exc}"
    store.update(rec)


@app.post("/v1/recordings", response_model=RecordingResponse)
async def create_recording(
    background_tasks: BackgroundTasks, file: UploadFile = File(...)
) -> RecordingResponse:
    recording_id = uuid4().hex
    filename = file.filename or f"{recording_id}.wav"
    if not _is_supported_audio_upload(filename, file.content_type):
        raise HTTPException(status_code=415, detail="unsupported audio upload")

    destination = _storage_dir() / f"{recording_id}{_safe_suffix(filename)}"

    size = 0
    with destination.open("wb") as out:
        while chunk := await file.read(1024 * 1024):
            size += len(chunk)
            if size > config.upload_max_bytes:
                destination.unlink(missing_ok=True)
                raise HTTPException(
                    status_code=413,
                    detail=f"uploaded file exceeds {config.upload_max_bytes} bytes",
                )
            out.write(chunk)

    if size == 0:
        destination.unlink(missing_ok=True)
        raise HTTPException(status_code=400, detail="uploaded file is empty")

    recording = Recording(
        id=recording_id,
        audio=AudioRef(
            path=destination,
            filename=filename,
            content_type=file.content_type,
            size_bytes=size,
            duration_seconds=_audio_duration_seconds(destination),
        ),
        status=JobStatus.RECEIVED,
    )
    store.add(recording)
    # Return now; the device polls GET for transcript/summary as they land.
    background_tasks.add_task(_process, recording_id)
    return _to_response(recording)


@app.get("/v1/recordings", response_model=list[RecordingResponse])
async def list_recordings() -> list[RecordingResponse]:
    return [_to_response(rec) for rec in store.list()]


@app.get("/v1/recordings/{recording_id}", response_model=RecordingResponse)
async def get_recording(recording_id: str) -> RecordingResponse:
    rec = store.get(recording_id)
    if rec is None:
        raise HTTPException(status_code=404, detail="recording not found")
    return _to_response(rec)


@app.get("/v1/recordings/{recording_id}/audio")
async def get_recording_audio(recording_id: str) -> FileResponse:
    # Path comes from the store keyed by id (never from the request), so there is
    # no user-controlled path to traverse.
    rec = store.get(recording_id)
    if rec is None or not rec.audio.path.exists():
        raise HTTPException(status_code=404, detail="audio not found")
    return FileResponse(
        rec.audio.path,
        media_type=_audio_media_type(rec),
        filename=rec.audio.filename,
    )


@app.get("/v1/recordings/{recording_id}/export.md")
async def export_recording_markdown(recording_id: str) -> PlainTextResponse:
    rec = store.get(recording_id)
    if rec is None:
        raise HTTPException(status_code=404, detail="recording not found")
    return PlainTextResponse(
        _render_markdown(rec),
        media_type="text/markdown",
        headers={"Content-Disposition": f'attachment; filename="{rec.id}.md"'},
    )


def _render_markdown(rec: Recording) -> str:
    lines: list[str] = [
        f"# {rec.audio.filename}",
        "",
        f"- **Status:** {rec.status.value}",
        f"- **Created:** {rec.created_at.isoformat()}",
        f"- **Length:** {_format_duration(_recording_duration_seconds(rec))}",
    ]
    if isinstance(rec.summary, Summary):
        lines += ["", "## Summary", "", rec.summary.summary]
        if rec.summary.action_items:
            lines += ["", "## Action items", ""]
            for item in rec.summary.action_items:
                meta = ", ".join(
                    part
                    for part in (
                        f"owner: {item.owner}" if item.owner else "",
                        f"due: {item.due}" if item.due else "",
                    )
                    if part
                )
                lines.append(f"- [ ] {item.text}" + (f" ({meta})" if meta else ""))
    if rec.transcript is not None and rec.transcript.text:
        lines += ["", "## Transcript", ""]
        if rec.transcript.segments:
            for segment in rec.transcript.segments:
                speaker = f"{segment.speaker}: " if segment.speaker else ""
                timestamp = ""
                if segment.start is not None:
                    timestamp = f"[{segment.start:.1f}s] "
                lines.append(f"{timestamp}{speaker}{segment.text}")
        else:
            lines.append(rec.transcript.text)
    if rec.error:
        lines += ["", "## Error", "", rec.error]
    return "\n".join(lines) + "\n"


@app.get("/", response_class=HTMLResponse)
async def index(request: Request) -> HTMLResponse:
    recordings: Iterable[RecordingResponse] = [_to_response(r) for r in store.list()]
    return templates.TemplateResponse(
        request,
        "index.html",
        {
            "recordings": recordings,
            "format_bytes": _format_bytes,
            "format_duration": _format_duration,
        },
    )


@app.get("/rec/{recording_id}", response_class=HTMLResponse)
async def recording_detail(request: Request, recording_id: str) -> HTMLResponse:
    rec = store.get(recording_id)
    if rec is None:
        raise HTTPException(status_code=404, detail="recording not found")
    return templates.TemplateResponse(
        request,
        "recording.html",
        {
            "rec": _to_response(rec),
            "format_bytes": _format_bytes,
            "format_duration": _format_duration,
        },
    )


@app.post("/rec/{recording_id}/reprocess")
async def reprocess_recording(
    recording_id: str, background_tasks: BackgroundTasks
) -> RedirectResponse:
    if store.get(recording_id) is None:
        raise HTTPException(status_code=404, detail="recording not found")
    background_tasks.add_task(_process, recording_id)
    return RedirectResponse(url=f"/rec/{recording_id}", status_code=303)
