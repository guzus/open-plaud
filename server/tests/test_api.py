from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from open_plaud.api.app import app, config, recordings


def test_create_recording_stores_upload(tmp_path: Path) -> None:
    config.storage_dir = tmp_path
    recordings.clear()
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("rec_0001.wav", b"RIFF....WAVEfmt ", "audio/wav")},
    )

    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "transcribing"
    assert body["filename"] == "rec_0001.wav"
    assert body["size_bytes"] == 16
    assert len(recordings) == 1
    stored = next((tmp_path / "audio").glob("*.wav"))
    assert stored.read_bytes() == b"RIFF....WAVEfmt "

    get_response = client.get(f"/v1/recordings/{body['id']}")
    assert get_response.status_code == 200
    assert get_response.json() == body


def test_create_recording_rejects_empty_upload(tmp_path: Path) -> None:
    config.storage_dir = tmp_path
    recordings.clear()
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("empty.wav", b"", "audio/wav")},
    )

    assert response.status_code == 400
    assert response.json() == {"detail": "uploaded file is empty"}
    assert not list((tmp_path / "audio").glob("*"))


def test_get_recording_missing_id() -> None:
    recordings.clear()
    client = TestClient(app)

    response = client.get("/v1/recordings/missing")

    assert response.status_code == 404
    assert response.json() == {"detail": "recording not found"}
