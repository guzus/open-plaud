from __future__ import annotations

from pathlib import Path
from uuid import uuid4

from fastapi import FastAPI, File, HTTPException, UploadFile
from pydantic import BaseModel, ConfigDict

from open_plaud.config import Config
from open_plaud.types import AudioRef, JobStatus, Recording


class RecordingResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: str
    status: JobStatus
    filename: str
    size_bytes: int


app = FastAPI(title="open-plaud API", version="0.0.1")
config = Config()
recordings: dict[str, Recording] = {}


def _storage_dir() -> Path:
    path = config.storage_dir / "audio"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _safe_suffix(filename: str | None) -> str:
    suffix = Path(filename or "").suffix.lower()
    return suffix if suffix in {".wav", ".flac", ".m4a", ".mp3"} else ".wav"


def _to_response(recording: Recording) -> RecordingResponse:
    return RecordingResponse(
        id=recording.id,
        status=recording.status,
        filename=recording.audio.filename,
        size_bytes=recording.audio.size_bytes,
    )


@app.post("/v1/recordings", response_model=RecordingResponse)
async def create_recording(file: UploadFile = File(...)) -> RecordingResponse:
    recording_id = uuid4().hex
    filename = file.filename or f"{recording_id}.wav"
    destination = _storage_dir() / f"{recording_id}{_safe_suffix(filename)}"

    size = 0
    with destination.open("wb") as out:
        while chunk := await file.read(1024 * 1024):
            size += len(chunk)
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
        ),
        status=JobStatus.TRANSCRIBING,
    )
    recordings[recording_id] = recording
    return _to_response(recording)


@app.get("/v1/recordings/{recording_id}", response_model=RecordingResponse)
async def get_recording(recording_id: str) -> RecordingResponse:
    recording = recordings.get(recording_id)
    if recording is None:
        raise HTTPException(status_code=404, detail="recording not found")
    return _to_response(recording)
