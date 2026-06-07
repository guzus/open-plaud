# Shopping list — Seoul / Korea

Where to actually buy each part if you're in Seoul. Prices are approximate
(₩, mid-2026); click the vendor search links for live prices.

> **Verified:** the reference board ([Waveshare ESP32-S3-AUDIO-Board](https://www.waveshare.com/esp32-s3-audio-board.htm),
> ~$18 / ~₩25k) **ships with a 3.7V LiPo battery included** and has an onboard
> microSD slot + charge management. So the real minimum build is just **two items**.

## TL;DR — minimum build (~₩32k)

| # | Part | Approx | Fastest in Seoul |
|---|---|---|---|
| 1 | Waveshare ESP32-S3-AUDIO-Board (battery incl.) | ~₩25–35k | Domestic distributor (1–3 days) or AliExpress (cheapest) |
| 2 | microSD card 32GB | ~₩7k | Coupang 로켓배송 (next day) |

Everything below is **optional** (screen, spare battery, mics, case, magnet).

## Full list

| Part | Buy where (Seoul) | Search term | Approx ₩ | Notes |
|---|---|---|---|---|
| **ESP32-S3-AUDIO-Board** | AliExpress *Waveshare Official Store* (cheapest, ~1–2 wk) · or domestic: 디바이스마트 / 엘레파츠 / 메카솔루션 / IC뱅큐 (1–3 days) | `ESP32-S3-AUDIO` or `Waveshare ESP32-S3 오디오` | 25–35k | Battery **included**. ESP32-S3R8 (8MB PSRAM/16MB flash), ES7210 ADC + ES8311 DAC, dual mic, microSD. |
| **microSD 8–32GB** | 쿠팡 (로켓배송) · 다이소 · 하이마트 | `마이크로SD 32GB` (SanDisk/삼성) | 6–9k | Format FAT32. 32GB is plenty for WAV. |
| **(opt) spare/bigger LiPo** | 디바이스마트 / 엘레파츠 · 세운상가 배터리상 · AliExpress | `리튬폴리머 3.7V 500mAh` | 5–8k | ⚠️ Match the board's connector + polarity before plugging in. The included cell already works — only buy if you want more capacity. |
| **(opt) SPI LCD 1.47"/2"/2.8"** | Same Waveshare channels | `Waveshare 2inch LCD SPI` | 8–15k | Board has an SPI LCD header. Screen-less (button + LED) is fine. |
| **(opt) MagSafe magnet ring** | 쿠팡 · 다이소 · AliExpress | `맥세이프 메탈링` | 3–5k | For card-style phone attach. |
| **(opt) I2S MEMS mics ×2–4** | 디바이스마트 / 엘레파츠 · AliExpress · 세운상가 | `INMP441` or `ICS-43434` | ~3k each | Only to extend pickup range; board already has a dual-mic array. Needs jumper wires + soldering. |
| **(opt) 3D-printed case** | 오프린트미(ohprintme) · 메이커스 · 세운상가 출력집 · Naver "3D프린팅 출력 서비스" | upload STL | 5–10k | ⚠️ Case STL isn't published in this repo yet (ROADMAP v1). Run screen-less in any small enclosure until then. |

## Vendors

| Vendor | Type | Link | Best for |
|---|---|---|---|
| 디바이스마트 (Devicemart) | Domestic online, 1–3 days | https://www.devicemart.co.kr | Dev boards, Waveshare, modules |
| 엘레파츠 (Eleparts) | Domestic online | https://www.eleparts.co.kr | Boards, batteries, connectors |
| 메카솔루션 (Mechasolution) | Domestic online | https://mechasolution.com | ESP32 / maker parts |
| IC뱅큐 (ICbanq) | Domestic online | https://www.icbanq.com | Boards, sensors |
| 쿠팡 (Coupang) | Domestic, next-day | https://www.coupang.com | microSD, magnet, cables |
| AliExpress — Waveshare Official Store | Import, ~1–2 wk | https://www.aliexpress.com (search "ESP32-S3-AUDIO-Board Waveshare") | Cheapest board + Waveshare LCD/parts |
| 세운상가 / 청계천 전자상가 | **Offline, Jongno-gu** — same day | — | Batteries, microSD, connectors, mics, jumper wire, hands-on |

## Tips for Seoul buyers

- **Want it this week:** buy the board domestically (디바이스마트/메카솔루션) even if a bit pricier; grab the microSD on Coupang for next-day.
- **Want it cheapest:** AliExpress Waveshare Official Store for the board (+ any LCD/mics in the same order to save shipping), ~1–2 weeks.
- **Want it today / hands-on:** 세운상가 (Sewoon Sangga, 종로) — bring the part names above; good for batteries, microSD, mics, wire, and sometimes the dev board itself.
- **Duty-free:** keep a single AliExpress order **under ₩150,000** (personal-use import threshold) to avoid customs duty. The whole build is well under that.
