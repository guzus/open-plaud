"""Build Open Plaud ADPCM containers exactly the way the firmware builds them.

This is the single canonical port of the *encoder* side. It is deliberately a
line-by-line port of `adpcm_encode_sample()` and `write_opad_header()` in
`firmware/main/net_upload.c`, **not** a re-use of the server decoder's tables run
in reverse. That is the whole point: tests built on this assert that the server
reproduces what the *device* produces, so drift in either implementation fails
here rather than in the field.

IMA ADPCM is lossy, but it is exactly reproducible: the encoder's running
predictor after each sample IS the sample the decoder must emit. So callers can
compare against the encoder's own predictor trail and demand bit-equality rather
than a tolerance.

Note what the firmware does with channels: `write_opad_header()` hard-codes the
container's channel count to 1 and `upload_file_adpcm()` averages L+R into mono
before encoding, so a real device only ever emits a **mono** container.
`build_container` still accepts multi-channel input because the decoder supports
it and that generality is worth testing, but `device_container()` is the shape
that actually comes off the wire.
"""

from __future__ import annotations

import math
import struct

OPADPCM_MAGIC = b"OPADPCM1"

# What `net_upload.c` puts in the multipart part for an ADPCM transfer. The
# filename is the *WAV* name on the SD card -- the firmware does not rename the
# part to .opadpcm -- and the vendor content type is the only wire signal that
# the bytes are not PCM. See docs/ARCHITECTURE.md.
DEVICE_MEDIA_TYPE = "application/vnd.openplaud.adpcm"
DEVICE_FILENAME = "rec_0001.wav"

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


class Encoder:
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


def build_container(
    channels_pcm: list[list[int]], *, sample_rate: int = 16000
) -> tuple[bytes, list[list[int]]]:
    """Encode per-channel PCM the way the firmware does; return (bytes, expected).

    `expected` is the encoder's predictor trail per channel — what a correct
    decoder must emit, sample for sample.
    """
    channels = len(channels_pcm)
    samples_per_channel = len(channels_pcm[0])
    encoders = [Encoder(pcm[0]) for pcm in channels_pcm]
    expected = [[pcm[0]] for pcm in channels_pcm]

    nibbles: list[int] = []
    for i in range(1, samples_per_channel):
        for channel in range(channels):
            nibbles.append(encoders[channel].encode(channels_pcm[channel][i]))
            expected[channel].append(encoders[channel].predictor)

    payload = bytearray()
    for i in range(0, len(nibbles), 2):
        low = nibbles[i]
        # The firmware flushes a dangling low nibble with a zero high nibble
        # (`if (have_low_nibble) checked_write(&pending_byte, 1)`).
        high = nibbles[i + 1] if i + 1 < len(nibbles) else 0
        payload.append(low | (high << 4))

    header = bytearray(OPADPCM_MAGIC)
    header += struct.pack("<IHI", sample_rate, channels, samples_per_channel)
    for pcm in channels_pcm:
        header += struct.pack("<hBB", pcm[0], 0, 0)
    return bytes(header) + bytes(payload), expected


def speech_like(n: int, *, seed: float = 0.0) -> list[int]:
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


def device_container(samples: int = 16000) -> tuple[bytes, list[int]]:
    """The exact container shape a stock device uploads: mono, 16 kHz, real audio.

    Returns (container bytes, expected decoded samples).
    """
    raw, expected = build_container([speech_like(samples)])
    return raw, expected[0]
