# Bill of materials — reference build

The reference target is the Waveshare ESP32-S3-AUDIO-Board: a dual-mic array
with an ES7210 4-channel ADC, chosen over the ePaper-1.54 board because mic
quality is the thing that matters most for a Plaud-style recorder.

| Part | Spec | Approx cost | Notes |
|---|---|---|---|
| Waveshare ESP32-S3-AUDIO-Board | ESP32-S3R8, 8MB PSRAM, 16MB Flash, dual digital mic + ES7210 ADC + ES8311 DAC, microSD, RTC, USB-C | ~₩25k ($16–18 Waveshare) | Get the "with battery" SKU |
| 3.7V LiPo (MX1.25) | ~500mAh | ~₩5–8k | Often bundled with board |
| microSD card | 8–32GB, FAT32 | ~₩7k | Records WAV here |
| (optional) SPI LCD | Waveshare 1.47"/2"/2.8" | ~₩8–15k | Status/UI; not e-ink |
| (optional) MagSafe magnet ring | — | ~₩5k | For card-style phone attach |
| 3D-printed case | card form factor | ~₩5–10k | Print service if no printer |

**Total: ~₩40–55k** for a better-mic build than the original Pala Note.

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
