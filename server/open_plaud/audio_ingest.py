"""Normalize device upload containers into something every provider can read.

The firmware compresses Wi-Fi transfers (`OPEN_PLAUD_COMPRESS_TRANSFERS`): a clip
arrives either as Ogg Opus, or — with Opus off — as the dependency-free **Open
Plaud ADPCM** container that `firmware/main/net_upload.c` writes. Neither is PCM
WAV, and `whisper_cpp` reads only 16-bit PCM WAV, so an ADPCM upload is decoded
back to canonical PCM here, at ingest, once. Downstream stages then never learn
that a transfer codec existed.

Opus is passed through untouched: it is a real container that ffmpeg-backed
providers (whisperx) and the OpenAI API already accept, and re-encoding it would
only lose quality.

Container layout (byte-for-byte the same as `server-go`'s decoder, so both
servers accept the same device):

    8 bytes   "OPADPCM1"
    uint32    sample rate, little-endian
    uint16    channel count (1 or 2)
    uint32    samples per channel, INCLUDING the predictor sample
    repeated per channel:
      int16   initial predictor PCM sample
      uint8   initial IMA step index
      uint8   reserved, must be 0
    nibbles   IMA ADPCM codes, low nibble first, interleaved by frame/channel
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import BinaryIO

OPADPCM_MAGIC = b"OPADPCM1"
OPADPCM_SUFFIX = ".opadpcm"
OPADPCM_MEDIA_TYPE = "application/vnd.openplaud.adpcm"

_FIXED_HEADER_BYTES = 18          # magic(8) + sample rate(4) + channels(2) + samples(4)
_STATE_BYTES_PER_CHANNEL = 4      # predictor int16 + step index uint8 + reserved uint8
_WAV_HEADER_BYTES = 44
_MAX_SAMPLE_RATE = 192_000
_MAX_CHANNELS = 2
_CHUNK_BYTES = 64 * 1024

# Standard IMA ADPCM tables. Identical to the firmware encoder's copies in
# net_upload.c / ble_audio.c — if one moves, all three move.
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


class OpenPlaudADPCMError(ValueError):
    """The upload claimed to be an Open Plaud ADPCM container but is not usable."""


def looks_like_opadpcm(path: Path) -> bool:
    """True if `path` starts with the container magic. Cheap enough to always run."""
    try:
        with path.open("rb") as handle:
            return handle.read(len(OPADPCM_MAGIC)) == OPADPCM_MAGIC
    except OSError:
        return False


def wav_header(sample_rate: int, channels: int, data_bytes: int) -> bytes:
    """Canonical 44-byte 16-bit PCM WAV header (matches the firmware's writer)."""
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + data_bytes,
        b"WAVE",
        b"fmt ",
        16,                                  # fmt chunk size
        1,                                   # PCM
        channels,
        sample_rate,
        sample_rate * channels * 2,          # byte rate
        channels * 2,                        # block align
        16,                                  # bits per sample
        b"data",
        data_bytes,
    )


class _ChannelState:
    """Per-channel IMA predictor/index. Stereo interleaves one state per channel."""

    __slots__ = ("predictor", "index")

    def __init__(self, predictor: int, index: int) -> None:
        self.predictor = predictor
        self.index = index


def _decode_nibble(state: _ChannelState, code: int) -> int:
    step = _STEP_TABLE[state.index]
    diff = step >> 3
    if code & 1:
        diff += step >> 2
    if code & 2:
        diff += step >> 1
    if code & 4:
        diff += step
    if code & 8:
        state.predictor -= diff
    else:
        state.predictor += diff
    if state.predictor > 32767:
        state.predictor = 32767
    elif state.predictor < -32768:
        state.predictor = -32768
    state.index += _INDEX_TABLE[code]
    if state.index < 0:
        state.index = 0
    elif state.index >= len(_STEP_TABLE):
        state.index = len(_STEP_TABLE) - 1
    return state.predictor


def _read_exactly(source: BinaryIO, count: int, what: str) -> bytes:
    data = source.read(count)
    if len(data) != count:
        raise OpenPlaudADPCMError(f"truncated Open Plaud ADPCM {what}")
    return data


def _read_header(source: BinaryIO) -> tuple[int, int, int, list[_ChannelState]]:
    fixed = _read_exactly(source, _FIXED_HEADER_BYTES, "header")
    if fixed[: len(OPADPCM_MAGIC)] != OPADPCM_MAGIC:
        raise OpenPlaudADPCMError("invalid Open Plaud ADPCM magic")

    sample_rate, channels, samples_per_channel = struct.unpack_from("<IHI", fixed, 8)
    if not 0 < sample_rate <= _MAX_SAMPLE_RATE:
        raise OpenPlaudADPCMError(f"invalid Open Plaud ADPCM sample rate {sample_rate}")
    if not 0 < channels <= _MAX_CHANNELS:
        raise OpenPlaudADPCMError(f"invalid Open Plaud ADPCM channel count {channels}")
    if samples_per_channel == 0:
        raise OpenPlaudADPCMError("invalid Open Plaud ADPCM empty audio")

    states: list[_ChannelState] = []
    raw_states = _read_exactly(source, channels * _STATE_BYTES_PER_CHANNEL, "state")
    for channel in range(channels):
        predictor, index, reserved = struct.unpack_from(
            "<hBB", raw_states, channel * _STATE_BYTES_PER_CHANNEL
        )
        if index >= len(_STEP_TABLE):
            raise OpenPlaudADPCMError(f"invalid Open Plaud ADPCM step index {index}")
        if reserved != 0:
            raise OpenPlaudADPCMError("invalid Open Plaud ADPCM reserved byte")
        states.append(_ChannelState(predictor, index))
    return sample_rate, channels, samples_per_channel, states


def decode_opadpcm(source: BinaryIO, destination: BinaryIO, *, max_output_bytes: int) -> int:
    """Stream one container into `destination` as PCM WAV; return bytes written.

    Strict on purpose, and strict the same way `server-go` is: a truncated or
    over-long payload raises instead of yielding half a clip. The device keeps
    the clip queued on SD until the upload is accepted, so failing loudly costs
    a retry, while silently storing a partial clip would lose audio for good.
    """
    sample_rate, channels, samples_per_channel, states = _read_header(source)

    data_bytes = samples_per_channel * channels * 2
    if data_bytes > 0xFFFFFFFF - 36:
        raise OpenPlaudADPCMError("Open Plaud ADPCM audio exceeds the WAV size limit")
    if _WAV_HEADER_BYTES + data_bytes > max_output_bytes:
        raise OpenPlaudADPCMError(
            f"decoded audio would exceed {max_output_bytes} bytes"
        )

    written = destination.write(wav_header(sample_rate, channels, data_bytes))
    # Sample 0 is carried literally in the header as each channel's predictor.
    written += destination.write(
        struct.pack(f"<{channels}h", *(state.predictor for state in states))
    )

    total_nibbles = (samples_per_channel - 1) * channels
    payload_bytes = (total_nibbles + 1) // 2      # trailing high nibble is padding
    channel = 0
    produced = 0
    remaining = payload_bytes

    while remaining > 0:
        chunk = source.read(min(_CHUNK_BYTES, remaining))
        if not chunk:
            raise OpenPlaudADPCMError("truncated Open Plaud ADPCM payload")
        remaining -= len(chunk)
        pcm = bytearray()
        for byte in chunk:
            for code in (byte & 0x0F, byte >> 4):
                if produced == total_nibbles:
                    break
                pcm += struct.pack("<h", _decode_nibble(states[channel], code))
                channel = (channel + 1) % channels
                produced += 1
        written += destination.write(pcm)

    if source.read(1):
        raise OpenPlaudADPCMError("invalid Open Plaud ADPCM trailing data")
    return written


def decode_opadpcm_file(source: Path, destination: Path, *, max_output_bytes: int) -> int:
    """File-to-file wrapper around `decode_opadpcm`. Returns bytes written."""
    with source.open("rb") as src, destination.open("wb") as dst:
        return decode_opadpcm(src, dst, max_output_bytes=max_output_bytes)
