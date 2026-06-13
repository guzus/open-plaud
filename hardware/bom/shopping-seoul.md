# Shopping list — Seoul / Korea

Where to actually buy each part if you're in Seoul. Prices are approximate
(₩, mid-2026); click the vendor search links for live prices.

> **Selected v0 hardware:** [Waveshare ESP32-S3-LCD-1.54 SKU 33867](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33867)
> **without touch control, with the 3.7V battery option**. Waveshare documents the LCD-1.54 and
> Touch-LCD-1.54 as sharing the same core features except touch: dual mic +
> ES7210 audio capture, ES8311, TF card, USB-C, Wi-Fi/BLE, and battery
> charge/discharge. Current Waveshare price observed: **$15.99 + shipping**.

## TL;DR — selected LCD-1.54 recorder (~₩45–70k before custom case)

| # | Part | Approx | Fastest in Seoul/Korea | Search term |
|---|---|---:|---|---|
| 1 | **Waveshare ESP32-S3-LCD-1.54 with battery** | Waveshare **$15.99 + shipping** · AliExpress ~₩26k | Waveshare official SKU 33867, or AliExpress if shipping is better | `ESP32-S3-LCD-1.54 Waveshare battery` |
| 2 | **microSD 64GB** | ~₩15–25k | 쿠팡 로켓배송 / 다이소 / 하이마트 | `마이크로SD 64GB 삼성 샌디스크` |
| 3 | **3D print case** | ~₩5–15k | 세운상가 출력집 or online 3D print service | First-article LCD-1.54 STL exists; use board-in-hand measurements for the second article |
| 4 | **USB-C cable** | ~₩3–10k | already likely owned / 쿠팡 / 다이소 | `USB C 케이블 데이터` |
| 5 | Optional MagSafe metal ring | ~₩3–5k | 쿠팡 / 다이소 | `맥세이프 메탈링` |

Buy the **battery-included SKU**, not the `-EN` no-battery SKU. This avoids
separate LiPo polarity/connector risk for the first recorder build.

Immediate caveat: the repo has first-article STLs for AUDIO-board, XIAO, and
LCD-1.54, but the LCD-1.54 shell is still based on published dimensions rather
than caliper data. The right next CAD step is to measure the LCD-1.54 board and
bundled battery once it arrives, then tune the case around real measurements.

> **Verified:** the [Waveshare ESP32-S3-AUDIO-Board](https://www.waveshare.com/esp32-s3-audio-board.htm)
> comes in **two SKUs — board-only ($16) and "+battery" ($18)**. The battery is an
> **MX1.25 2-pin** 3.7V LiPo and is **not optional** for a portable recorder. The board is
> **58 × 43.7 mm**, so you also need a **case**. Repo STLs exist in `hardware/case/stl/`,
> but they are first-article prints that should be measured and tuned.

> **Verified:** [Seeed XIAO ESP32-S3 Sense](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
> is **$13.99** direct from Seeed. Seeed documents the Sense stack as
> **21 × 17.8 × 15 mm** with camera, digital mic, and SD-card support.

## Alternative — thin XIAO prototype (~₩40–70k)

This is the thinner path we discussed: **XIAO ESP32-S3 Sense + flat LiPo directly
soldered to the battery pads**, inside the new STL case. It is smaller than the
Waveshare reference build, but still a stacked dev-board prototype.

| # | Part | Approx | Fastest in Seoul/Korea | Search term |
|---|---|---:|---|---|
| 1 | **Seeed XIAO ESP32-S3 Sense** — preferably unsoldered headers | Seeed $13.99 · domestic ~₩22–24k | 디바이스마트 / IC뱅큐 / 엘레파츠 if in stock; otherwise Seeed or AliExpress | `XIAO ESP32S3 Sense 113991115` |
| 2 | **Flat 3.7V LiPo, protected preferred** | ~₩8–12k | 엑스캅터 / 배터리맨 / 컴스마트 / 세운상가 배터리상 | `리튬폴리머 3.7V 500mAh 503035` or `902030` |
| 3 | **microSD 32GB** | ~₩8–15k | 쿠팡 로켓배송 / 다이소 / 하이마트 | `마이크로SD 32GB 삼성 샌디스크` |
| 4 | **Kapton tape** | ~₩3–5k | 쿠팡 / 세운상가 / 디바이스마트 | `캡톤 테이프 10mm` |
| 5 | **30AWG silicone wire** red/black | ~₩3–8k | 세운상가 / 디바이스마트 / 엘레파츠 | `30AWG 실리콘 와이어` |
| 6 | **3D print case** | ~₩5–15k | 세운상가 출력집 or online 3D print service | upload `open_plaud_xiao_lipo_base.stl` + `open_plaud_xiao_lipo_lid.stl` |

New STL files:

- [`hardware/case/stl/open_plaud_xiao_lipo_base.stl`](../case/stl/open_plaud_xiao_lipo_base.stl)
- [`hardware/case/stl/open_plaud_xiao_lipo_lid.stl`](../case/stl/open_plaud_xiao_lipo_lid.stl)

Generated size: **39.8 × 34.8 × 24.7 mm assembled**. The thickness is driven by
the XIAO Sense stack height, not the battery.

Important buying notes:

- Do **not** buy the Grove battery base for this STL. The whole point is lower bulk.
- Buy the **unsoldered** XIAO if possible; pre-soldered pin headers make the case
  harder to use.
- The battery connector on random LiPo packs may be wrong or unnecessary. For
  this prototype, you can cut/strip leads and solder directly, but polarity must
  be verified before power-up.
- XIAO Sense uses a single digital mic. If microphone quality is the priority,
  the Waveshare dual-mic board remains the better audio reference.

## Alternative — Waveshare AUDIO-board recorder (~₩55–70k)

A device you carry needs **board + battery + microSD + case** — all four are essential:

| # | Part | Approx | Fastest in Seoul |
|---|---|---|---|
| 1 | ESP32-S3-AUDIO-Board — buy the **"+battery" SKU** | ~₩25–35k | Domestic distributor (1–3 days) or AliExpress (cheapest) |
| 2 | microSD **64GB** (SanDisk Ultra / Samsung EVO) | ~₩20k | Coupang 로켓배송 (next day) |
| 3 | Case (3D print, fits 58×43.7 mm) | ~₩5–10k | STL in `hardware/case/` — print & measure |
| — | 3.7V MX1.25 LiPo | (in the +battery SKU) | Bundled when you pick that SKU |

Genuinely **optional** extras: SPI screen, MagSafe magnet, extra I2S mics.

## Full list

| Part | Buy where (Seoul) | Search term | Approx ₩ | Notes |
|---|---|---|---|---|
| **ESP32-S3-LCD-1.54 with battery** | [Waveshare official SKU 33867](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33867) · AliExpress if shipping is cheaper | `ESP32-S3-LCD-1.54 Waveshare`, `ESP32-S3-Touch-LCD-1.54` | $15.99 + shipping / ~₩26k on AliExpress | **Primary v0 board.** Dual mic + ES7210, ES8311, TF, battery charging, LCD status. Prefer non-touch. |
| **XIAO ESP32-S3 Sense** | Seeed official (cheapest direct) · domestic: 디바이스마트 / IC뱅큐 / 엘레파츠 | `XIAO ESP32S3 Sense 113991115` | 22–24k domestic / $13.99 Seeed | Thin prototype board. Official dimension: 21×17.8×15 mm with Sense expansion board. |
| **Flat 3.7V LiPo** | 엑스캅터 / 배터리맨 / 컴스마트 / 세운상가 배터리상 | `리튬폴리머 3.7V 500mAh 503035` | 8–12k | Protected cell preferred. Direct-solder only after checking +/− with a meter. |
| **Kapton tape + 30AWG silicone wire** | 쿠팡 / 디바이스마트 / 엘레파츠 / 세운상가 | `캡톤 테이프`, `30AWG 실리콘 와이어` | 6–13k | Needed for insulation and strain relief in the direct-LiPo build. |
| **ESP32-S3-AUDIO-Board** | AliExpress *Waveshare Official Store* (cheapest, ~1–2 wk) · or domestic: 디바이스마트 / 엘레파츠 / 메카솔루션 / IC뱅큐 (1–3 days) | `ESP32-S3-AUDIO` or `Waveshare ESP32-S3 오디오` | 25–35k | Pick the **+battery SKU** (+~$2). ESP32-S3R8 (8MB PSRAM/16MB flash), ES7210 ADC + ES8311 DAC, dual mic, microSD, 58×43.7 mm. |
| **microSD 64–128GB** | 쿠팡 (로켓배송) · 다이소 · 하이마트 | `마이크로SD 64GB 샌디스크` | 20–30k | FAT32. ⚠️ 32GB is a dying capacity (~₩15–22k) — barely cheaper than 64GB, so size up. 64GB ≈ 500h of WAV. |
| **3.7V LiPo (MX1.25)** — required | Easiest: the board's **+battery SKU**. Separately: 디바이스마트 / 엘레파츠 · 세운상가 배터리상 · AliExpress | `리튬폴리머 3.7V MX1.25` | 5–8k | ⚠️ Must be **MX1.25 2-pin**, correct polarity — wrong polarity can kill the board. Buying the +battery SKU avoids this. Bigger mAh = longer recording. |
| **(opt) SPI LCD 1.47"/2"/2.8"** | Same Waveshare channels | `Waveshare 2inch LCD SPI` | 8–15k | Board has an SPI LCD header. Screen-less (button + LED) is fine. |
| **(opt) MagSafe magnet ring** | 쿠팡 · 다이소 · AliExpress | `맥세이프 메탈링` | 3–5k | For card-style phone attach. |
| **(opt) I2S MEMS mics ×2–4** | 디바이스마트 / 엘레파츠 · AliExpress · 세운상가 | `INMP441` or `ICS-43434` | ~3k each | Only to extend pickup range; board already has a dual-mic array. Needs jumper wires + soldering. |
| **Case** — required (carry) | 3D-print service: 오프린트미(ohprintme, online upload→택배) · 메이커스 · 세운상가 출력집 (Naver "3D프린팅 출력 서비스") | upload the STL | 5–10k | ✅ Parametric STL now in [`hardware/case/`](../case/) (`uv run … generate_case.py`). Cutout positions are estimates — print the base, measure with calipers, tweak params, reprint. |

## Vendors

| Vendor | Type | Link | Best for |
|---|---|---|---|
| 디바이스마트 (Devicemart) | Domestic online, 1–3 days | https://www.devicemart.co.kr | Dev boards, Waveshare, modules |
| 엘레파츠 (Eleparts) | Domestic online | https://www.eleparts.co.kr | Boards, batteries, connectors |
| 메카솔루션 (Mechasolution) | Domestic online | https://mechasolution.com | ESP32 / maker parts |
| IC뱅큐 (ICbanq) | Domestic online | https://www.icbanq.com | Boards, sensors |
| 쿠팡 (Coupang) | Domestic, next-day | https://www.coupang.com | microSD, magnet, cables |
| AliExpress — Waveshare Official Store | Import, ~1–2 wk | https://www.aliexpress.com (search "ESP32-S3-LCD-1.54 Waveshare") | Cheapest Waveshare board options |
| 세운상가 / 청계천 전자상가 | **Offline, Jongno-gu** — same day | — | Batteries, microSD, connectors, mics, jumper wire, hands-on |

## Tips for Seoul buyers

- **Want it this week:** buy the LCD-1.54 board domestically if listed; grab the microSD on Coupang for next-day.
- **Want it cheapest:** Waveshare/AliExpress official channels for the LCD-1.54 battery SKU, ~1–2 weeks.
- **Want it today / hands-on:** 세운상가 (Sewoon Sangga, 종로) — bring the part names above; good for batteries, microSD, mics, wire, and sometimes the dev board itself.
- **Duty-free:** keep a single AliExpress order **under ₩150,000** (personal-use import threshold) to avoid customs duty. The whole build is well under that.
