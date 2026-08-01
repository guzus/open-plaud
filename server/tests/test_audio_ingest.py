"""Open Plaud ADPCM ingest tests.

The encoder here is a line-by-line port of `adpcm_encode_sample()` in
`firmware/main/net_upload.c`, not a re-use of the decoder's tables in reverse.
That is the whole point: these tests assert that the server reproduces what the
*device* produces, so a drift in either implementation fails here rather than in
the field.

IMA ADPCM is lossy, but it is exactly reproducible: the encoder's running
predictor after each sample IS the sample the decoder must emit. So the tests
compare against the encoder's own predictor trail and demand bit-equality, not
a tolerance.
"""

from __future__ import annotations

import io
import math
import struct
import wave
from pathlib import Path

import pytest

from open_plaud.audio_ingest import (
    OPADPCM_MAGIC,
    OpenPlaudADPCMError,
    decode_opadpcm,
    decode_opadpcm_file,
    looks_like_opadpcm,
)

_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


class _Encoder:
    """Port of the firmware's `adpcm_encode_sample()` (net_upload.c)."""

    def __init__(self, predictor: int) -> None:
        self.predictor = predictor
        self.index = 0

    def encode(self, sample: int) -> int:
        step = _STEP_TABLE[self.index]
        diff = sample - self.predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        if diff >= (step >> 1):
            code |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= (step >> 2):
            code |= 1
            delta += step >> 2
        if code & 8:
            self.predictor -= delta
        else:
            self.predictor += delta
        self.predictor = max(-32768, min(32767, self.predictor))
        self.index += _INDEX_TABLE[code & 0x0F]
        self.index = max(0, min(len(_STEP_TABLE) - 1, self.index))
        return code & 0x0F


def _build_container(
    channels_pcm: list[list[int]], *, sample_rate: int = 16000
) -> tuple[bytes, list[list[int]]]:
    """Encode per-channel PCM the way the firmware does; return (bytes, expected).

    `expected` is the encoder's predictor trail per channel — what a correct
    decoder must emit, sample for sample.
    """
    channels = len(channels_pcm)
    samples_per_channel = len(channels_pcm[0])
    encoders = [_Encoder(pcm[0]) for pcm in channels_pcm]
    expected = [[pcm[0]] for pcm in channels_pcm]

    nibbles: list[int] = []
    for i in range(1, samples_per_channel):
        for channel in range(channels):
            nibbles.append(encoders[channel].encode(channels_pcm[channel][i]))
            expected[channel].append(encoders[channel].predictor)

    payload = bytearray()
    for i in range(0, len(nibbles), 2):
        low = nibbles[i]
        high = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        payload.append(low | (high << 4))

    header = bytearray(OPADPCM_MAGIC)
    header += struct.pack("<IHI", sample_rate, channels, samples_per_channel)
    for pcm in channels_pcm:
        header += struct.pack("<hBB", pcm[0], 0, 0)
    return bytes(header) + bytes(payload), expected


def _speech_like(n: int, *, seed: float = 0.0) -> list[int]:
    # Two summed tones plus a slow envelope: enough dynamic range to exercise the
    # step-index adaptation in both directions, unlike a constant or a ramp.
    out: list[int] = []
    for i in range(n):
        envelope = 0.35 + 0.65 * abs(math.sin((i + seed) / 90.0))
        value = envelope * (
            9000 * math.sin((i + seed) / 7.0) + 4000 * math.sin((i + seed) / 23.0)
        )
        out.append(max(-32768, min(32767, int(value))))
    return out


def _decode(raw: bytes, *, max_output_bytes: int = 8 * 1024 * 1024) -> bytes:
    out = io.BytesIO()
    decode_opadpcm(io.BytesIO(raw), out, max_output_bytes=max_output_bytes)
    return out.getvalue()


def _wav_samples(raw: bytes) -> tuple[wave._wave_params, list[int]]:  # type: ignore[name-defined]
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
