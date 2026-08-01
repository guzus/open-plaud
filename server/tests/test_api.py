from __future__ import annotations

import io
import struct
import wave
from collections.abc import Iterator
from pathlib import Path

import opadpcm_fixtures
import pytest
from fastapi.testclient import TestClient

from open_plaud.api.app import app, config, store
from open_plaud.types import (
    ActionItem,
    AudioRef,
    Summary,
    TranscribeOptions,
    Transcript,
    TranscriptSegment,
)


class _StubTranscriber:
    """Returns content-bearing text so the orchestrator's summarize branch runs."""

    name = "stub"

    async def transcribe(self, audio: AudioRef, opts: TranscribeOptions) -> Transcript:
        return Transcript(
            text="hello world",
            language="en",
            segments=[TranscriptSegment(text="hello world", start=0.0, end=1.5)],
        )


class _StubSummarizer:
    name = "stub"

    async def summarize(
        self, transcript: Transcript, *, instructions: str | None = None
    ) -> Summary:
        return Summary(
            summary="A concise summary.",
            action_items=[ActionItem(text="email Bob", owner="Alice", due=None)],
        )


def _use_stub_pipeline(monkeypatch: pytest.MonkeyPatch) -> None:
    # _process reads the module-level `pipeline`, so patching its stages here makes
    # the real upload->pipeline->persist->response path run with non-empty content.
    from open_plaud.api import app as app_module

    monkeypatch.setattr(app_module.pipeline, "transcriber", _StubTranscriber())
    monkeypatch.setattr(app_module.pipeline, "summarizer", _StubSummarizer())


@pytest.fixture(autouse=True)
def _isolate(tmp_path: Path) -> Iterator[None]:
    # Point both the audio blobs and the SQLite DB at a fresh tmp dir per test.
    # store.reset() drops the cached connection so the next op reconnects under
    # the new storage_dir (mirrors the lazy-path contract in store.py).
    original_storage_dir = config.storage_dir
    original_upload_max_bytes = config.upload_max_bytes
    config.storage_dir = tmp_path
    config.upload_max_bytes = 512 * 1024 * 1024
    store.reset()
    yield
    config.storage_dir = original_storage_dir
    config.upload_max_bytes = original_upload_max_bytes
    store.reset()


def _post_wav(client: TestClient, name: str = "rec_0001.wav", data: bytes = b"RIFFdata") -> str:
    response = client.post("/v1/recordings", files={"file": (name, data, "audio/wav")})
    assert response.status_code == 200
    return str(response.json()["id"])


def _wav_bytes(*, seconds: int = 1, sample_rate: int = 8000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"\x00\x00" * sample_rate * seconds)
    return buf.getvalue()


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
    assert body["duration_seconds"] is None

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


def test_create_recording_rejects_unsupported_upload(tmp_path: Path) -> None:
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("notes.txt", b"not audio", "text/plain")},
    )

    assert response.status_code == 415
    assert response.json() == {"detail": "unsupported audio upload"}
    assert not (tmp_path / "audio").exists()


def test_create_recording_rejects_oversized_upload(tmp_path: Path) -> None:
    config.upload_max_bytes = 4
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("rec.wav", b"12345", "audio/wav")},
    )

    assert response.status_code == 413
    assert response.json() == {"detail": "uploaded file exceeds 4 bytes"}
    assert not list((tmp_path / "audio").glob("*"))


def test_get_recording_missing_id() -> None:
    client = TestClient(app)

    response = client.get("/v1/recordings/missing")

    assert response.status_code == 404
    assert response.json() == {"detail": "recording not found"}


def test_healthz_returns_ok() -> None:
    client = TestClient(app)

    response = client.get("/healthz")

    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


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


def test_recording_duration_is_returned_and_rendered() -> None:
    client = TestClient(app)
    rid = _post_wav(client, name="one-second.wav", data=_wav_bytes(seconds=1))

    got = client.get(f"/v1/recordings/{rid}")
    assert got.status_code == 200
    assert got.json()["duration_seconds"] == 1.0

    listing = client.get("/v1/recordings")
    assert listing.status_code == 200
    listed = next(item for item in listing.json() if item["id"] == rid)
    assert listed["duration_seconds"] == 1.0

    index = client.get("/")
    assert index.status_code == 200
    assert "<th>Length</th>" in index.text
    assert "0:01" in index.text

    md = client.get(f"/v1/recordings/{rid}/export.md")
    assert md.status_code == 200
    assert "- **Length:** 0:01" in md.text


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
    response = client.post("/v1/recordings", files={"file": ("x.wav", b"RIFFdata", "text/html")})
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


def test_full_pipeline_produces_transcript_and_summary(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # The core value path: upload -> transcribe -> summarize -> response/export/UI,
    # exercised with content (the default noop providers produce empty text, which
    # skips summarization, so this is the only test that runs the summary branch).
    _use_stub_pipeline(monkeypatch)
    client = TestClient(app)
    rid = _post_wav(client, name="meeting.wav")

    got = client.get(f"/v1/recordings/{rid}").json()
    assert got["status"] == "done"
    assert got["transcript"] == {
        "text": "hello world",
        "language": "en",
        "segments": [{"text": "hello world", "start": 0.0, "end": 1.5, "speaker": None}],
    }
    assert got["summary"]["summary"] == "A concise summary."
    assert got["summary"]["action_items"][0] == {
        "text": "email Bob",
        "owner": "Alice",
        "due": None,
    }

    md = client.get(f"/v1/recordings/{rid}/export.md").text
    assert "A concise summary." in md
    assert "email Bob" in md

    detail = client.get(f"/rec/{rid}").text
    assert "A concise summary." in detail
    assert "email Bob" in detail


def test_reprocess_reruns_pipeline(monkeypatch: pytest.MonkeyPatch) -> None:
    _use_stub_pipeline(monkeypatch)
    client = TestClient(app)
    rid = _post_wav(client)

    redirect = client.post(f"/rec/{rid}/reprocess", follow_redirects=False)
    assert redirect.status_code == 303
    assert redirect.headers["location"] == f"/rec/{rid}"

    got = client.get(f"/v1/recordings/{rid}").json()
    assert got["status"] == "done"
    assert got["summary"]["summary"] == "A concise summary."


def _opadpcm_bytes(*, samples: int = 16000, sample_rate: int = 16000) -> bytes:
    """Smallest valid Open Plaud ADPCM container: silence, one channel.

    All-zero nibbles decode to a flat zero signal (step 7 >> 3 == 0), which is
    all this needs — bit-exactness against the firmware encoder is covered in
    tests/test_audio_ingest.py.
    """
    header = b"OPADPCM1" + struct.pack("<IHI", sample_rate, 1, samples)
    header += struct.pack("<hBB", 0, 0, 0)  # predictor, step index, reserved
    return header + bytes(samples // 2)


def test_create_recording_decodes_open_plaud_adpcm(tmp_path: Path) -> None:
    # The firmware uploads this container whenever Opus is off but
    # OPEN_PLAUD_COMPRESS_TRANSFERS is on. It must land as PCM WAV, because
    # whisper_cpp reads nothing else.
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={
            "file": (
                "rec_0007.opadpcm",
                _opadpcm_bytes(),
                "application/vnd.openplaud.adpcm",
            )
        },
    )

    assert response.status_code == 200
    body = response.json()
    assert body["filename"] == "rec_0007.wav"
    assert body["size_bytes"] == 44 + 16000 * 2
    assert body["duration_seconds"] == 1.0

    stored = list((tmp_path / "audio").glob("*"))
    assert [path.suffix for path in stored] == [".wav"]
    with wave.open(str(stored[0]), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 16000
        assert wav.getnframes() == 16000


def test_create_recording_decodes_adpcm_sent_under_a_wav_filename(tmp_path: Path) -> None:
    """The real device wire shape: `rec_NNNN.wav` carrying ADPCM bytes.

    This is what `net_upload.c` actually sends, and it is the case the other
    ADPCM tests miss. `upload_file_adpcm()` names the part after the *WAV* on the
    SD card (`const char *filename = strrchr(path, '/')`) and only switches the
    content type to the vendor value — it never renames the part to `.opadpcm`.
    With `OPEN_PLAUD_PREPARE_OPUS_SIDECAR` defaulting to `n`, this is the path a
    stock build takes for every clip.

    Two things make this test discriminating where the `.opadpcm` ones are not:

    1. `_safe_suffix("rec_0001.wav")` is `.wav`, so the upload is stored at the
       very path `_unwrap_opadpcm` decodes *to* — source and destination are the
       same file. Only the temp-file+rename keeps that from truncating the
       source mid-read. A `.opadpcm` upload never exercises it.
    2. Asserting the exact decoded length and the full sample trail (not just a
       `RIFF` prefix) means a decoder that stops early still fails.
    """
    raw, expected = opadpcm_fixtures.device_container(samples=16000)
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={
            "file": (
                opadpcm_fixtures.DEVICE_FILENAME,
                raw,
                opadpcm_fixtures.DEVICE_MEDIA_TYPE,
            )
        },
    )

    assert response.status_code == 200
    body = response.json()
    assert body["filename"] == "rec_0001.wav"
    assert body["size_bytes"] == 44 + 16000 * 2
    assert body["duration_seconds"] == 1.0

    # The vendor content type must not be persisted: what is on disk is now WAV.
    persisted = store.get(body["id"])
    assert persisted is not None
    assert persisted.audio.content_type == "audio/wav"

    stored = list((tmp_path / "audio").glob("*"))
    assert [path.suffix for path in stored] == [".wav"], "no .tmp scratch may survive"
    assert stored[0].stat().st_size == 44 + 16000 * 2

    with wave.open(str(stored[0]), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 16000
        assert wav.getnframes() == 16000
        frames = wav.readframes(wav.getnframes())
    assert list(struct.unpack(f"<{len(frames) // 2}h", frames)) == expected


def test_create_recording_rejects_adpcm_suffix_without_container(tmp_path: Path) -> None:
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("rec_0008.opadpcm", b"not a container", "audio/wav")},
    )

    assert response.status_code == 400
    assert response.json() == {"detail": "upload is not an Open Plaud ADPCM container"}
    assert not list((tmp_path / "audio").glob("*"))


def test_create_recording_rejects_corrupt_adpcm_container(tmp_path: Path) -> None:
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={
            "file": (
                "rec_0009.opadpcm",
                _opadpcm_bytes()[:-500],  # truncated mid-payload
                "application/vnd.openplaud.adpcm",
            )
        },
    )

    assert response.status_code == 400
    assert "invalid Open Plaud ADPCM upload" in response.json()["detail"]
    assert not list((tmp_path / "audio").glob("*"))


def test_create_recording_accepts_ogg_opus(tmp_path: Path) -> None:
    # Regression guard for the shipped firmware default (OPEN_PLAUD_OPUS_TRANSFERS
    # = y): before this was accepted, every sync from a default-built device was
    # rejected with 415 by this server.
    client = TestClient(app)

    response = client.post(
        "/v1/recordings",
        files={"file": ("rec_0010.opus", b"OggS\x00\x02fake-opus", "audio/ogg")},
    )

    assert response.status_code == 200
    body = response.json()
    assert body["filename"] == "rec_0010.opus"

    stored = list((tmp_path / "audio").glob("*"))
    assert [path.suffix for path in stored] == [".opus"]

    download = client.get(f"/v1/recordings/{body['id']}/audio")
    assert download.status_code == 200
    assert download.headers["content-type"] == "audio/ogg"
