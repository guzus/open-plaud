# Bill of materials — reference build

The reference target is the Waveshare ESP32-S3-AUDIO-Board: a dual-mic array
with an ES7210 4-channel ADC, chosen over the ePaper-1.54 board because mic
quality is the thing that matters most for a Plaud-style recorder.

| Part | Spec | Approx cost | Notes |
|---|---|---|---|
| Waveshare ESP32-S3-AUDIO-Board | ESP32-S3R8, 8MB PSRAM, 16MB Flash, dual mic + ES7210 ADC + ES8311 DAC, microSD, USB-C, MX1.25 LiPo connector + charge mgmt, 58×43.7 mm | ~₩22k ($16 board-only) / ~₩25k ($18 +battery) | Two SKUs — pick **"+battery"** to bundle the cell |
| 3.7V LiPo battery — **required** (portable) | ~500 mAh, **MX1.25 2-pin**, correct polarity | ~₩5–8k | Bundled if you chose the +battery SKU; else buy one |
| microSD card | 64–128GB, FAT32 | ~₩20–30k | 32GB barely cheaper (~₩2만) — size up; records WAV here |
| Case — **required** (carry) | fits 58×43.7 mm board, mic/speaker/USB cutouts | ~₩5–10k | 3D-print custom; **no repo STL yet** (ROADMAP v1) |
| (optional) SPI LCD | Waveshare 1.47"/2"/2.8" | ~₩8–15k | Status/UI; not e-ink |
| (optional) MagSafe magnet ring | — | ~₩5k | For card-style phone attach |

**Portable recorder (essential): board + battery + microSD + case ≈ ₩55–70k.**
Screen, magnet, and extra mics are the genuinely optional extras.
Still a better-mic build than the original Pala Note.

> 📍 In Seoul/Korea? See **[shopping-seoul.md](./shopping-seoul.md)** for exact vendors,
> Korean search terms, and the fastest vs. cheapest buying paths.

## Why not the ePaper-1.54 board?

Single MEMS mic — fine for close memos, weak for meetings. The AUDIO-Board's
dual-mic array with noise reduction / echo cancellation is the upgrade that
matters for Plaud-like use. Trade-off: no onboard e-ink (add an SPI LCD or run
screen-less with button + LED feedback).

## Mic upgrade path

For longer pickup, add 2–4 I2S MEMS mics (INMP441 / ICS-43434, ~₩3k each) in a
beamforming array. See `hardware/wiring/`.

## Sourcing (Korea)

- Devicemart / Eleparts — domestic, 1–3 days
- AliExpress (official Waveshare store) — cheapest, ~1–2 weeks
- Coupang — hit or miss for this board

Stay under ₩150,000 for duty-free personal import.
