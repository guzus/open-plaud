"""Open Plaud ADPCM ingest tests.

The container builder lives in `tests/opadpcm_fixtures.py` and is a line-by-line
port of the *firmware encoder*, not a re-use of the decoder's tables in reverse.
That is the whole point: these tests assert that the server reproduces what the
*device* produces, so a drift in either implementation fails here rather than in
the field. See that module's docstring for the reasoning.
"""

from __future__ import annotations

import io
import struct
import wave
from pathlib import Path

import pytest
from opadpcm_fixtures import OPADPCM_MAGIC, build_container as _build_container
from opadpcm_fixtures import speech_like as _speech_like

from open_plaud.audio_ingest import (
    OpenPlaudADPCMError,
    decode_opadpcm,
    decode_opadpcm_file,
    looks_like_opadpcm,
)


def _decode(raw: bytes, *, max_output_bytes: int = 8 * 1024 * 1024) -> bytes:
    out = io.BytesIO()
    decode_opadpcm(io.BytesIO(raw), out, max_output_bytes=max_output_bytes)
    return out.getvalue()


def _wav_samples(raw: bytes) -> tuple[wave._wave_params, list[int]]:
    with wave.open(io.BytesIO(raw), "rb") as wav:
        params = wav.getparams()
        frames = wav.readframes(wav.getnframes())
    count = len(frames) // 2
    return params, list(struct.unpack(f"<{count}h", frames))


@pytest.mark.parametrize("samples", [2, 3, 64, 65, 1000])
def test_mono_round_trip_is_bit_exact(samples: int) -> None:
    pcm = _speech_like(samples)
    raw, expected = _build_container([pcm])

    params, decoded = _wav_samples(_decode(raw))

    assert params.nchannels == 1
    assert params.sampwidth == 2
    assert params.framerate == 16000
    assert params.nframes == samples
    assert decoded == expected[0]


def test_odd_sample_count_pads_the_final_nibble() -> None:
    # 65 samples -> 64 nibbles -> 32 bytes; 64 samples -> 63 nibbles -> 32 bytes
    # with a padding high nibble the decoder must ignore rather than emit.
    pcm = _speech_like(64)
    raw, expected = _build_container([pcm])
    _, decoded = _wav_samples(_decode(raw))
    assert len(decoded) == 64
    assert decoded == expected[0]


def test_stereo_round_trip_interleaves_by_frame() -> None:
    left = _speech_like(200)
    right = _speech_like(200, seed=11.0)
    raw, expected = _build_container([left, right])

    params, decoded = _wav_samples(_decode(raw))

    assert params.nchannels == 2
    assert params.nframes == 200
    assert decoded[0::2] == expected[0]
    assert decoded[1::2] == expected[1]


def test_decoded_wav_is_readable_by_the_stdlib_and_sized_correctly() -> None:
    pcm = _speech_like(16000)
    raw, _ = _build_container([pcm])
    decoded = _decode(raw)

    # 4x expansion is the whole point of shipping ADPCM over the radio.
    assert len(decoded) == 44 + 16000 * 2
    assert len(raw) == 22 + 8000
    params, _samples = _wav_samples(decoded)
    assert params.nframes / params.framerate == pytest.approx(1.0)


def test_looks_like_opadpcm_sniffs_magic(tmp_path: Path) -> None:
    good = tmp_path / "clip.opadpcm"
    good.write_bytes(_build_container([_speech_like(32)])[0])
    bad = tmp_path / "clip.wav"
    bad.write_bytes(b"RIFF....WAVEfmt ")
    missing = tmp_path / "nope.opadpcm"

    assert looks_like_opadpcm(good) is True
    assert looks_like_opadpcm(bad) is False
    assert looks_like_opadpcm(missing) is False


def test_decode_file_writes_pcm_wav(tmp_path: Path) -> None:
    pcm = _speech_like(500)
    raw, expected = _build_container([pcm])
    src = tmp_path / "clip.opadpcm"
    src.write_bytes(raw)
    dst = tmp_path / "clip.wav"

    written = decode_opadpcm_file(src, dst, max_output_bytes=8 * 1024 * 1024)

    assert written == dst.stat().st_size == 44 + 500 * 2
    _, decoded = _wav_samples(dst.read_bytes())
    assert decoded == expected[0]


def test_rejects_bad_magic() -> None:
    raw = bytearray(_build_container([_speech_like(32)])[0])
    raw[0:8] = b"NOTOPAD1"
    with pytest.raises(OpenPlaudADPCMError, match="magic"):
        _decode(bytes(raw))


def test_rejects_truncated_payload() -> None:
    raw, _ = _build_container([_speech_like(200)])
    with pytest.raises(OpenPlaudADPCMError, match="truncated"):
        _decode(raw[: len(raw) - 10])


def test_rejects_trailing_data() -> None:
    raw, _ = _build_container([_speech_like(64)])
    with pytest.raises(OpenPlaudADPCMError, match="trailing"):
        _decode(raw + b"\x00\x00")


def test_rejects_nonzero_reserved_byte() -> None:
    raw = bytearray(_build_container([_speech_like(32)])[0])
    raw[21] = 1  # reserved byte of the single channel's state
    with pytest.raises(OpenPlaudADPCMError, match="reserved"):
        _decode(bytes(raw))


def test_rejects_out_of_range_step_index() -> None:
    raw = bytearray(_build_container([_speech_like(32)])[0])
    raw[20] = 89  # step table has 89 entries: valid indices are 0..88
    with pytest.raises(OpenPlaudADPCMError, match="step index"):
        _decode(bytes(raw))


@pytest.mark.parametrize(
    ("field", "value", "match"),
    [
        ("rate", 0, "sample rate"),
        ("rate", 192001, "sample rate"),
        ("channels", 0, "channel count"),
        ("channels", 3, "channel count"),
        ("samples", 0, "empty audio"),
    ],
)
def test_rejects_impossible_header_fields(field: str, value: int, match: str) -> None:
    raw = bytearray(_build_container([_speech_like(32)])[0])
    rate, channels, samples = struct.unpack_from("<IHI", raw, 8)
    if field == "rate":
        rate = value
    elif field == "channels":
        channels = value
    else:
        samples = value
    struct.pack_into("<IHI", raw, 8, rate, channels, samples)
    with pytest.raises(OpenPlaudADPCMError, match=match):
        _decode(bytes(raw))


def test_rejects_output_larger_than_the_cap() -> None:
    raw, _ = _build_container([_speech_like(4000)])
    with pytest.raises(OpenPlaudADPCMError, match="exceed"):
        _decode(raw, max_output_bytes=1024)


def test_rejects_header_shorter_than_the_fixed_prefix() -> None:
    with pytest.raises(OpenPlaudADPCMError, match="truncated"):
        _decode(bytes(OPADPCM_MAGIC) + b"\x00\x00")
