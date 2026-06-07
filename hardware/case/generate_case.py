# /// script
# requires-python = ">=3.11"
# dependencies = ["build123d>=0.10,<0.11"]
# ///
"""
open-plaud — parametric case for the Waveshare ESP32-S3-AUDIO-Board.

Two printable parts (base tray + friction-fit lid), generated with build123d
(pure-Python CAD — no GUI, runs straight from uv):

    uv run --no-project hardware/case/generate_case.py

Outputs ASCII STL to ./stl/.

⚠️  Cutout POSITIONS are estimates. Waveshare publishes only the 58 × 43.7 mm PCB
    *outline* plus a round speaker-HOUSING STEP — NOT a per-edge connector drawing.
    So treat the first print as a "first article": print the base, measure your
    board with calipers, adjust the *_POS_* parameters below, and reprint.
    Everything here is parametric on purpose.

Verified board facts (see hardware/case/README.md for sources):
    PCB 58 × 43.7 mm · USB-C (native USB, no UART bridge) · microSD · 2× mic ·
    7× WS2812B RGB ring · ES7210 ADC + ES8311 DAC · MX1.25 LiPo connector.
"""
from pathlib import Path

from build123d import Box, Cylinder, Pos, export_stl

# ---- board (confirmed Waveshare spec) -----------------------------------
PCB_L = 58.0      # X
PCB_W = 43.7      # Y
PCB_T = 1.6       # PCB thickness

# ---- case shell ----------------------------------------------------------
WALL = 2.0
CLR = 0.5         # gap around the PCB on each side
FLOOR = 1.5
STANDOFF_H = 6.0  # gap under the PCB for the MX1.25 LiPo (~5 mm thick)
CAVITY_H = 16.0   # interior height above the floor (PCB + components + headroom)

INNER_L = PCB_L + 2 * CLR
INNER_W = PCB_W + 2 * CLR
OUTER_L = INNER_L + 2 * WALL
OUTER_W = INNER_W + 2 * WALL
BASE_H = FLOOR + CAVITY_H

# ---- lid -----------------------------------------------------------------
LID_TOP = 1.5
LIP_H = 4.0
LIP_WALL = 1.5
LID_FIT_CLR = 0.4   # rim slides inside the tray walls (friction fit)

# ---- standoffs -----------------------------------------------------------
SO_R = 2.5
SO_HOLE_R = 1.0     # pilot hole for an M2 self-tapping screw (optional)
SO_INSET = 3.5      # corner standoff inset from the PCB edge

# ---- cutouts (⚠️ MEASURE — these positions are estimates) ----------------
USBC_W, USBC_H = 9.5, 3.6                      # USB-C opening (Y width, Z height)
USBC_POS_Z = FLOOR + STANDOFF_H + PCB_T + 0.6  # ~ connector centre above PCB
SD_W, SD_H = 12.0, 2.0                         # microSD slot
SD_POS_Z = FLOOR + STANDOFF_H + PCB_T
MIC_R = 0.9                                    # two mic pin-holes in the lid
MIC_POS = [(-9.0, 14.0), (9.0, 14.0)]
GRILLE_R = 1.1                                 # speaker grille (optional)
GRILLE_CENTER = (0.0, -12.0)
GRILLE_GRID = 3                                # GRILLE_GRID × GRILLE_GRID holes
GRILLE_STEP = 4.0

OUT = Path(__file__).resolve().parent / "stl"


def base_tray():
    """Open-top tray: floor + walls, corner standoffs, USB-C + microSD edge slots."""
    part = Pos(0, 0, BASE_H / 2) * Box(OUTER_L, OUTER_W, BASE_H)

    # hollow from the top, leaving a floor of thickness FLOOR
    part -= Pos(0, 0, FLOOR + BASE_H / 2) * Box(INNER_L, INNER_W, BASE_H)

    # four corner standoffs to seat the PCB above the battery, each with a pilot hole
    sx, sy = PCB_L / 2 - SO_INSET, PCB_W / 2 - SO_INSET
    for x in (-sx, sx):
        for y in (-sy, sy):
            post = Pos(x, y, FLOOR + STANDOFF_H / 2) * Cylinder(SO_R, STANDOFF_H)
            # pilot hole stays inside the post (offset +0.5 keeps the floor intact)
            post -= Pos(x, y, FLOOR + STANDOFF_H / 2 + 0.5) * Cylinder(SO_HOLE_R, STANDOFF_H)
            part += post

    # USB-C on the +X short edge, microSD on the -X short edge (boxes pierce the wall)
    part -= Pos(OUTER_L / 2, 0, USBC_POS_Z) * Box(WALL * 4, USBC_W, USBC_H)
    part -= Pos(-OUTER_L / 2, 0, SD_POS_Z) * Box(WALL * 4, SD_W, SD_H)
    return part


def lid():
    """Top cap with a downward friction-fit rim, mic holes and a speaker grille."""
    part = Pos(0, 0, LID_TOP / 2) * Box(OUTER_L, OUTER_W, LID_TOP)

    # downward rim that nests inside the tray cavity
    rl, rw = INNER_L - LID_FIT_CLR, INNER_W - LID_FIT_CLR
    rim = Pos(0, 0, -LIP_H / 2) * Box(rl, rw, LIP_H)
    rim -= Pos(0, 0, -LIP_H / 2) * Box(rl - 2 * LIP_WALL, rw - 2 * LIP_WALL, LIP_H + 1)
    part += rim

    # mic pin-holes
    for mx, my in MIC_POS:
        part -= Pos(mx, my, LID_TOP / 2) * Cylinder(MIC_R, LID_TOP * 4)

    # optional speaker grille (square grid of holes)
    off = (GRILLE_GRID - 1) / 2
    for i in range(GRILLE_GRID):
        for j in range(GRILLE_GRID):
            gx = GRILLE_CENTER[0] + (i - off) * GRILLE_STEP
            gy = GRILLE_CENTER[1] + (j - off) * GRILLE_STEP
            part -= Pos(gx, gy, LID_TOP / 2) * Cylinder(GRILLE_R, LID_TOP * 4)
    return part


def main():
    OUT.mkdir(exist_ok=True)
    parts = {
        "open_plaud_case_base.stl": base_tray(),
        "open_plaud_case_lid.stl": lid(),
    }
    for name, solid in parts.items():
        path = OUT / name
        assert solid.volume > 0, f"{name}: empty solid — check parameters"
        export_stl(solid, str(path), ascii_format=True)
        bb = solid.bounding_box()
        size = bb.size
        print(
            f"wrote {path.name:30s} {path.stat().st_size:>8d} B  "
            f"bbox {size.X:.1f}×{size.Y:.1f}×{size.Z:.1f} mm"
        )
    print(
        f"assembled outer footprint ~ {OUTER_L:.1f} × {OUTER_W:.1f} × "
        f"{BASE_H + LID_TOP:.1f} mm"
    )


if __name__ == "__main__":
    main()
