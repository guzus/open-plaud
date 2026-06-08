# Bill of materials — reference build

The v0 reference target is **Waveshare ESP32-S3-LCD-1.54 + battery**. It wins the
current trade-off: under $30 direct, battery/charging included, TF/microSD,
Wi-Fi, small LCD status UI, and the same important audio architecture class:
dual mic + ES7210 ADC.

The older AUDIO-Board remains useful as an audio reference, and the XIAO direct
LiPo path remains useful as a tiny-body experiment, but neither is the main v0
buy anymore.

| Part | Spec | Approx cost | Notes |
|---|---|---|---|
| Waveshare ESP32-S3-LCD-1.54 **with battery** | ESP32-S3R8, 8MB PSRAM, 16MB Flash, dual mic + ES7210 ADC, ES8311, 1.54" LCD, TF/microSD, USB-C, battery charge/discharge | $15.99 direct + shipping / ~₩25–35k via AliExpress | Buy Waveshare SKU **33867**: without touch control + 3.7V battery included, not the `-EN` no-battery SKU |
| microSD card | 64–128GB, FAT32 | ~₩20–30k | Records WAV locally; 64GB is the value floor |
| Case — **required** (carry) | fits LCD-1.54 board + bundled battery, mic/speaker/USB/TF cutouts | ~₩5–15k | New LCD-1.54 STL still TODO; print after board arrives and dimensions are measured |
| (optional) MagSafe magnet ring | — | ~₩5k | For card-style phone attach |

**Portable recorder (essential): board-with-battery + shipping + microSD ≈ ₩45–70k before any custom case.**
Magnet is the only genuinely optional extra for the v0 carry build.

## Alternatives kept for reference

| Path | Approx cost | Why keep it | Why not primary |
|---|---:|---|---|
| ESP32-S3-AUDIO-Board + battery | ~$17.99 / ~₩25–35k | Best pure audio reference; current case STL exists | Bulky, no built-in status display |
| XIAO ESP32-S3 Sense + direct LiPo | ~$17–22 electronics | Small footprint; case STL exists | Single digital mic and stacked module; worse meeting audio |
| ESP32-S3-ePaper-1.54 + battery | ~$15.99–22.99 | Better always-on low-power status UI | Single mic / weaker recorder path |

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

Single mic + ES8311 is fine for close memos, weak for meetings. LCD-1.54 keeps
the useful small display while adding the recorder-grade part we care about:
dual mic + ES7210 capture.

## Mic upgrade path

For longer pickup, add 2–4 I2S MEMS mics (INMP441 / ICS-43434, ~₩3k each) in a
beamforming array. See `hardware/wiring/`.

## Sourcing (Korea)

- Devicemart / Eleparts — domestic, 1–3 days
- AliExpress (official Waveshare store) — cheapest, ~1–2 weeks
- Coupang — hit or miss for this board

Stay under ₩150,000 for duty-free personal import.
