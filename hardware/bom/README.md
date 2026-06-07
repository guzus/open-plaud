# Bill of materials — reference build

The reference target is the Waveshare ESP32-S3-AUDIO-Board: a dual-mic array
with an ES7210 4-channel ADC, chosen over the ePaper-1.54 board because mic
quality is the thing that matters most for a Plaud-style recorder.

There is also a thinner **XIAO ESP32-S3 Sense + direct LiPo** prototype path.
It is smaller and faster to carry-test, but it uses a single digital mic and a
stacked camera/mic module, so it is not the final PLAUD-thin hardware.

| Part | Spec | Approx cost | Notes |
|---|---|---|---|
| Waveshare ESP32-S3-AUDIO-Board | ESP32-S3R8, 8MB PSRAM, 16MB Flash, dual mic + ES7210 ADC + ES8311 DAC, microSD, USB-C, MX1.25 LiPo connector + charge mgmt, 58×43.7 mm | ~₩22k ($16 board-only) / ~₩25k ($18 +battery) | Two SKUs — pick **"+battery"** to bundle the cell |
| 3.7V LiPo battery — **required** (portable) | ~500 mAh, **MX1.25 2-pin**, correct polarity | ~₩5–8k | Bundled if you chose the +battery SKU; else buy one |
| microSD card | 64–128GB, FAT32 | ~₩20–30k | 32GB barely cheaper (~₩2만) — size up; records WAV here |
| Case — **required** (carry) | fits 58×43.7 mm board, mic/speaker/USB cutouts | ~₩5–10k | Parametric STL in [`hardware/case/`](../case/) — print first-article + measure |
| (optional) SPI LCD | Waveshare 1.47"/2"/2.8" | ~₩8–15k | Status/UI; not e-ink |
| (optional) MagSafe magnet ring | — | ~₩5k | For card-style phone attach |

**Portable recorder (essential): board + battery + microSD + case ≈ ₩55–70k.**
Screen, magnet, and extra mics are the genuinely optional extras.
Still a better-mic build than the original Pala Note.

## Thin prototype path — XIAO ESP32-S3 Sense

| Part | Spec | Approx cost | Notes |
|---|---|---:|---|
| Seeed XIAO ESP32-S3 Sense | ESP32-S3, Wi-Fi/BLE, camera, digital mic, SD slot, 21×17.8×15 mm with Sense expansion board | $13.99 / domestic ~₩22–24k | Buy unsoldered if available; pre-soldered headers add bulk |
| Flat 3.7V LiPo | 503035-class, ~500 mAh, protected preferred | ~₩8–12k | Direct-solder red/black leads to XIAO battery pads; verify polarity |
| microSD | 32GB FAT for safest XIAO bring-up | ~₩8–15k | XIAO docs mention 32GB FAT support; test larger cards later |
| Kapton tape + 30AWG silicone wire | insulation + strain relief | ~₩3–8k | Required for safe direct battery wiring |
| Case | `hardware/case/stl/open_plaud_xiao_lipo_*.stl` | ~₩5–15k print | First-article STL; measure and iterate |

**XIAO carry prototype electronics ≈ ₩35–55k before printing.**

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
