from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from open_plaud.api.app import app, config, store


@pytest.fixture(autouse=True)
def _isolate(tmp_path: Path) -> Iterator[None]:
    # Point both the audio blobs and the SQLite DB at a fresh tmp dir per test.
    # store.reset() drops the cached connection so the next op reconnects under
    # the new storage_dir (mirrors the lazy-path contract in store.py).
    config.storage_dir = tmp_path
    store.reset()
    yield
    store.reset()


def _post_wav(client: TestClient, name: str = "rec_0001.wav", data: bytes = b"RIFFdata") -> str:
    response = client.post("/v1/recordings", files={"file": (name, data, "audio/wav")})
    assert response.status_code == 200
    return str(response.json()["id"])


def test_create_recording_stores_upload(tmp_path: Path) -> None:
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("rec_0001.wav", b"RIFF....WAVEfmt ", "audio/wav")},
    )

    assert response.status_code == 200
    body = response.json()
    # Upload returns immediately; the pipeline runs in the background.
    assert body["status"] == "received"
    assert body["filename"] == "rec_0001.wav"
    assert body["size_bytes"] == 16

    stored = store.get(body["id"])
    assert stored is not None

    audio_file = next((tmp_path / "audio").glob("*.wav"))
    assert audio_file.read_bytes() == b"RIFF....WAVEfmt "

    # The background task (noop providers) completed before TestClient returned,
    # so the recording is now done with an (empty) transcript.
    get_response = client.get(f"/v1/recordings/{body['id']}")
    assert get_response.status_code == 200
    got = get_response.json()
    assert got["id"] == body["id"]
    assert got["status"] == "done"


def test_create_recording_rejects_empty_upload(tmp_path: Path) -> None:
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("empty.wav", b"", "audio/wav")},
    )

    assert response.status_code == 400
    assert response.json() == {"detail": "uploaded file is empty"}
    assert not list((tmp_path / "audio").glob("*"))


def test_get_recording_missing_id() -> None:
    client = TestClient(app)

    response = client.get("/v1/recordings/missing")

    assert response.status_code == 404
    assert response.json() == {"detail": "recording not found"}


def test_recording_survives_new_store_instance(tmp_path: Path) -> None:
    # Persistence: a recording uploaded now is visible to a store reconnecting to
    # the same DB file (proving it is on disk, not just in memory).
    from open_plaud.store import RecordingStore

    client = TestClient(app)
    rid = _post_wav(client)

    fresh = RecordingStore(config)
    assert fresh.get(rid) is not None
    fresh.reset()


def test_list_returns_uploaded_recordings() -> None:
    client = TestClient(app)
    rid = _post_wav(client)

    listing = client.get("/v1/recordings")
    assert listing.status_code == 200
    assert any(r["id"] == rid for r in listing.json())


def test_index_and_detail_pages_render() -> None:
    client = TestClient(app)
    rid = _post_wav(client, name="meeting.wav")

    index = client.get("/")
    assert index.status_code == 200
    assert "meeting.wav" in index.text

    detail = client.get(f"/rec/{rid}")
    assert detail.status_code == 200
    assert "meeting.wav" in detail.text


def test_audio_download_returns_bytes() -> None:
    client = TestClient(app)
    rid = _post_wav(client, data=b"RIFFpayload")

    audio = client.get(f"/v1/recordings/{rid}/audio")
    assert audio.status_code == 200
    assert audio.content == b"RIFFpayload"


def test_audio_download_is_attachment_with_safe_type() -> None:
    # A malicious upload content-type must never be reflected back as html; the
    # response must be an attachment served as a known audio type.
    client = TestClient(app)
    response = client.post(
        "/v1/recordings", files={"file": ("x.wav", b"RIFFdata", "text/html")}
    )
    rid = response.json()["id"]

    audio = client.get(f"/v1/recordings/{rid}/audio")
    assert audio.status_code == 200
    assert "attachment" in audio.headers["content-disposition"]
    assert audio.headers["content-type"].startswith("audio/")


def test_export_markdown() -> None:
    client = TestClient(app)
    rid = _post_wav(client, name="standup.wav")

    md = client.get(f"/v1/recordings/{rid}/export.md")
    assert md.status_code == 200
    assert md.text.startswith("# standup.wav")
    assert md.headers["content-type"].startswith("text/markdown")
