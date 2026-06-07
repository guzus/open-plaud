# Case — Waveshare ESP32-S3-AUDIO-Board

A parametric, 3D-printable two-part case (base tray + friction-fit lid) for the
reference board. Generated with **[build123d](https://build123d.readthedocs.io/)**
(pure-Python CAD), so it stays in the repo's `uv` toolchain — no GUI, no extra
system deps.

## Generate the STLs

```bash
uv run --no-project hardware/case/generate_case.py
```

`uv` installs build123d into an ephemeral env and writes two ASCII STLs to
`hardware/case/stl/`. Committed copies are already there:

| File | Part | Facets |
|---|---|---|
| `stl/open_plaud_case_base.stl` | tray (PCB sits on 4 standoffs above the battery) | ~4,100 |
| `stl/open_plaud_case_lid.stl` | lid (rim friction-fit, mic holes, speaker grille) | ~5,632 |

Assembled footprint ≈ **63 × 48.7 × 19 mm**.

## ⚠️ Cutout positions are estimates — print, measure, reprint

Waveshare publishes only the **58 × 43.7 mm PCB outline** plus a STEP/DXF of the
**round Ø58 speaker *housing*** (in [`ESP32-S3-AUDIO-Board.rar`](https://docs.waveshare.com/ESP32-S3-AUDIO-Board/Resources-And-Documents))
— **not** a per-edge connector drawing for the bare rectangular PCB. So the shell
(58 × 43.7) is reliable, but the **USB-C / microSD / mic / grille positions are
educated guesses.**

Workflow: print the **base** as a first article → seat your board → measure the
real connector offsets with calipers → edit the `*_POS_*` / `USBC_*` / `SD_*`
parameters at the top of `generate_case.py` → regenerate → reprint. It's
parametric precisely so this loop is cheap.

## Key parameters (`generate_case.py`)

| Param | Default | Meaning |
|---|---|---|
| `PCB_L`, `PCB_W` | 58, 43.7 | board outline (confirmed) |
| `WALL`, `FLOOR` | 2.0, 1.5 | shell thickness |
| `STANDOFF_H` | 6.0 | gap under PCB for the MX1.25 LiPo |
| `CAVITY_H` | 16.0 | interior height (PCB + parts + headroom) |
| `LID_FIT_CLR` | 0.4 | lid-rim slip fit — loosen/tighten to taste |
| `USBC_*`, `SD_*`, `MIC_POS`, `GRILLE_*` | — | **measure these** |

## Print settings (starting point)

PLA or PETG · 0.2 mm layer · 3 perimeters · 15–20% infill · **no supports**
(both parts print flat-side-down). If the lid is too tight/loose, change
`LID_FIT_CLR` and regenerate — don't sand.

## Why build123d (and the alternative)

build123d won here because the case is a build artifact that should stay in this
repo's `uv`/Python world and generate headlessly in CI. If you'd rather use a
mature, feature-rich enclosure DSL, **[YAPP_Box](https://github.com/mrWheel/YAPP_Box)**
(OpenSCAD, MIT, ~490★) ships standoffs, snap-joints, light tubes and ESP-board
examples — a great option if you already live in OpenSCAD. Avoid *Ultimate Box
Maker* for this project: it's CC BY-**NC**, incompatible with open-plaud's
build-and-sell-it ethos.

> Designing a case for a different board? See the **`parametric-enclosure`** skill.
