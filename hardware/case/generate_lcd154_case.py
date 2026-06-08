# /// script
# requires-python = ">=3.11"
# dependencies = ["build123d>=0.10,<0.11"]
# ///
"""
open-plaud — first-article case for Waveshare ESP32-S3-LCD-1.54 + battery.

This is a printable prototype enclosure for the selected v0 hardware target:
Waveshare SKU 33867 (without touch, 3.7V battery included). It is not a clone of
the vendor shell; it is a practical two-piece carry case designed around the
published product class and the observed integrated-battery layout.

    uv run --no-project hardware/case/generate_lcd154_case.py

Outputs ASCII STL to ./stl/.

Mechanical assumptions to verify with calipers after the board arrives:
    Outer product class: ~46 × 46 × 22.5 mm
    First-article printed shell target: 49 × 49 × 23 mm assembled
    Battery fits inside under/behind the board; pocket assumes ~38 × 32 × 5.8 mm
    LCD, USB-C, TF slot, mic, and button cutouts are generous starter positions.
"""
from pathlib import Path

from build123d import Box, Cylinder, Pos, export_stl

# ---- published/observed product class -----------------------------------
TARGET_PRODUCT_L = 46.0
TARGET_PRODUCT_W = 46.0
TARGET_PRODUCT_H = 22.5

# ---- case shell ----------------------------------------------------------
OUTER_L = 49.0
OUTER_W = 49.0
WALL = 1.6
FLOOR = 1.2
BASE_H = 21.5
LID_TOP = 1.5
LIP_H = 3.2
LIP_WALL = 1.0
LID_FIT_CLR = 0.35

INNER_L = OUTER_L - 2 * WALL
INNER_W = OUTER_W - 2 * WALL

# ---- internal layout -----------------------------------------------------
BAT_L = 38.0
BAT_W = 32.0
BAT_H = 5.8

BOARD_L = 42.5
BOARD_W = 42.5
BOARD_SUPPORT_Z = FLOOR + BAT_H + 1.0
BOARD_SUPPORT_H = 1.0
BOARD_SUPPORT_W = 2.0

# ---- front/top features --------------------------------------------------
LCD_WINDOW_L = 30.5
LCD_WINDOW_W = 30.5
LCD_WINDOW_Y = 4.0

MIC_R = 0.9
MIC_POS = [(-16.0, 17.0), (16.0, 17.0)]

BUTTON_R = 2.1
BUTTON_POS = [(-11.0, -18.0), (0.0, -18.0), (11.0, -18.0)]

# ---- side cutouts --------------------------------------------------------
USB_C_W = 9.5
USB_C_H = 4.0
USB_C_Z = 12.0
USB_C_Y = 0.0

TF_W = 13.0
TF_H = 2.5
TF_Z = 12.0
TF_Y = 0.0

SPEAKER_GRILLE_R = 0.9
SPEAKER_GRILLE_CENTER = (0.0, -8.0)
SPEAKER_GRILLE_STEP = 3.0
SPEAKER_GRILLE_GRID = 3

OUT = Path(__file__).resolve().parent / "stl"


def base_tray():
    """Open-top tray with battery pocket, board ledges, USB-C and TF cutouts."""
    part = Pos(0, 0, BASE_H / 2) * Box(OUTER_L, OUTER_W, BASE_H)
    part -= Pos(0, 0, FLOOR + BASE_H / 2) * Box(INNER_L, INNER_W, BASE_H)

    # Battery pocket: shallow negative feature so the pouch locates without being squeezed.
    part -= Pos(0, 0, FLOOR + BAT_H / 2) * Box(BAT_L + 0.6, BAT_W + 0.6, BAT_H + 0.2)

    # Four board support rails. Use foam tape or small printed shims after measuring.
    rail_z = BOARD_SUPPORT_Z + BOARD_SUPPORT_H / 2
    rail_x = BOARD_L / 2 + BOARD_SUPPORT_W / 2
    for x in (-rail_x, rail_x):
        part += Pos(x, 0, rail_z) * Box(BOARD_SUPPORT_W, BOARD_W, BOARD_SUPPORT_H)
    rail_y = BOARD_W / 2 + BOARD_SUPPORT_W / 2
    for y in (-rail_y, rail_y):
        part += Pos(0, y, rail_z) * Box(BOARD_L, BOARD_SUPPORT_W, BOARD_SUPPORT_H)

    # A small wire relief groove keeps battery leads from being pinched.
    part -= Pos(0, -INNER_W / 2 + 4.0, BOARD_SUPPORT_Z + 0.4) * Box(18.0, 4.0, 2.0)

    # USB-C on +X edge, TF/microSD on -X edge. Positions are first-article estimates.
    part -= Pos(OUTER_L / 2, USB_C_Y, USB_C_Z) * Box(WALL * 4, USB_C_W, USB_C_H)
    part -= Pos(-OUTER_L / 2, TF_Y, TF_Z) * Box(WALL * 4, TF_W, TF_H)

    # Tiny bottom vents near the battery pocket. Keep them small for printability.
    for x in (-10.0, 0.0, 10.0):
        part -= Pos(x, 16.0, FLOOR / 2) * Cylinder(0.9, FLOOR * 4)
    return part


def lid():
    """Friction-fit cap with LCD window, mic holes, button holes, and speaker grille."""
    part = Pos(0, 0, LID_TOP / 2) * Box(OUTER_L, OUTER_W, LID_TOP)

    rim_l = INNER_L - LID_FIT_CLR
    rim_w = INNER_W - LID_FIT_CLR
    rim = Pos(0, 0, -LIP_H / 2) * Box(rim_l, rim_w, LIP_H)
    rim -= Pos(0, 0, -LIP_H / 2) * Box(
        rim_l - 2 * LIP_WALL, rim_w - 2 * LIP_WALL, LIP_H + 0.5
    )
    part += rim

    part -= Pos(0, LCD_WINDOW_Y, LID_TOP / 2) * Box(
        LCD_WINDOW_L, LCD_WINDOW_W, LID_TOP * 4
    )

    for mx, my in MIC_POS:
        part -= Pos(mx, my, LID_TOP / 2) * Cylinder(MIC_R, LID_TOP * 4)

    for bx, by in BUTTON_POS:
        part -= Pos(bx, by, LID_TOP / 2) * Cylinder(BUTTON_R, LID_TOP * 4)

    off = (SPEAKER_GRILLE_GRID - 1) / 2
    for i in range(SPEAKER_GRILLE_GRID):
        for j in range(SPEAKER_GRILLE_GRID):
            gx = SPEAKER_GRILLE_CENTER[0] + (i - off) * SPEAKER_GRILLE_STEP
            gy = SPEAKER_GRILLE_CENTER[1] + (j - off) * SPEAKER_GRILLE_STEP
            part -= Pos(gx, gy, LID_TOP / 2) * Cylinder(SPEAKER_GRILLE_R, LID_TOP * 4)
    return part


def main():
    OUT.mkdir(exist_ok=True)
    parts = {
        "open_plaud_lcd154_base.stl": base_tray(),
        "open_plaud_lcd154_lid.stl": lid(),
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
    print(
        f"reference product class ~ {TARGET_PRODUCT_L:.1f} × {TARGET_PRODUCT_W:.1f} × "
        f"{TARGET_PRODUCT_H:.1f} mm"
    )


if __name__ == "__main__":
    main()
