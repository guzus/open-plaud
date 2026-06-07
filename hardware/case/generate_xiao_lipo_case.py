# /// script
# requires-python = ">=3.11"
# dependencies = ["build123d>=0.10,<0.11"]
# ///
"""
open-plaud — slim first-article case for Seeed XIAO ESP32-S3 Sense + flat LiPo.

This is a prototype enclosure for the "direct-solder LiPo to XIAO battery pads"
hardware path. It intentionally avoids the Grove battery base to keep the stack
smaller.

    uv run --no-project hardware/case/generate_xiao_lipo_case.py

Outputs ASCII STL to ./stl/.

Mechanical assumptions to verify with calipers before printing a second article:
    XIAO Sense stack: 21 × 17.8 × 15 mm (official Seeed dimension)
    LiPo pouch target: 503035 class, ~35 × 30 × 5.5 mm
    USB-C, mic, camera, and SD-slot openings are generous first-article cutouts.
"""
from pathlib import Path

from build123d import Box, Cylinder, Pos, export_stl

# ---- electronics ---------------------------------------------------------
XIAO_L = 21.0
XIAO_W = 17.8
XIAO_STACK_H = 15.0

BAT_L = 35.0
BAT_W = 30.0
BAT_H = 5.5

# ---- case shell ----------------------------------------------------------
WALL = 1.6
FLOOR = 1.2
CLR = 0.8
LID_TOP = 1.2
LIP_H = 3.0
LIP_WALL = 1.0
LID_FIT_CLR = 0.35

WIRE_CHANNEL_W = 3.0
WIRE_CHANNEL_H = 1.8

INNER_L = max(BAT_L, XIAO_L) + 2 * CLR
INNER_W = max(BAT_W, XIAO_W) + 2 * CLR
OUTER_L = INNER_L + 2 * WALL
OUTER_W = INNER_W + 2 * WALL

# Battery is under the board. The top clearance is intentionally high because
# the Sense board is a stack, not a flat PCB.
CAVITY_H = BAT_H + 1.0 + XIAO_STACK_H + 0.8
BASE_H = FLOOR + CAVITY_H

# ---- feature positions ---------------------------------------------------
USB_C_W = 10.0
USB_C_H = 4.0
USB_C_Z = FLOOR + BAT_H + 1.0 + 2.2

# Front-face camera/mic window in the lid. These are broad access openings for
# a first print; shrink once the real module orientation is measured.
CAMERA_WINDOW_L = 9.0
CAMERA_WINDOW_W = 9.0
MIC_R = 1.0
MIC_POS_X = 6.0
MIC_POS_Y = 0.0

BUTTON_R = 1.2
BUTTON_POS = [(-7.0, -6.5), (7.0, -6.5)]

OUT = Path(__file__).resolve().parent / "stl"


def base_tray():
    """Open-top tray with a battery pocket, XIAO ledge, USB-C cutout, and wire channel."""
    part = Pos(0, 0, BASE_H / 2) * Box(OUTER_L, OUTER_W, BASE_H)
    part -= Pos(0, 0, FLOOR + BASE_H / 2) * Box(INNER_L, INNER_W, BASE_H)

    # Shallow battery pocket keeps the pouch centered without squeezing it.
    pocket_z = FLOOR + BAT_H / 2
    part -= Pos(0, 0, pocket_z) * Box(BAT_L + 0.5, BAT_W + 0.5, BAT_H + 0.2)

    # Four tiny ledges support the XIAO board above the battery. Use Kapton foam
    # tape or a dab of non-conductive adhesive for retention; XIAO has no holes.
    ledge_z = FLOOR + BAT_H + 0.8
    ledge_h = 1.0
    ledge_w = 2.0
    lx = XIAO_L / 2 + ledge_w / 2
    for x in (-lx, lx):
        part += Pos(x, 0, ledge_z + ledge_h / 2) * Box(ledge_w, XIAO_W + 1.0, ledge_h)

    # Channel for the two battery wires, from battery pocket to XIAO battery pads.
    part -= Pos(0, -INNER_W / 2 + WIRE_CHANNEL_W, ledge_z) * Box(
        XIAO_L + 4.0, WIRE_CHANNEL_W, WIRE_CHANNEL_H
    )

    # USB-C cutout on the short edge. Print and trim/file if your connector sits off-center.
    part -= Pos(OUTER_L / 2, 0, USB_C_Z) * Box(WALL * 4, USB_C_W, USB_C_H)
    return part


def lid():
    """Friction-fit cap with camera/mic openings and reset/boot pin access."""
    part = Pos(0, 0, LID_TOP / 2) * Box(OUTER_L, OUTER_W, LID_TOP)

    rim_l = INNER_L - LID_FIT_CLR
    rim_w = INNER_W - LID_FIT_CLR
    rim = Pos(0, 0, -LIP_H / 2) * Box(rim_l, rim_w, LIP_H)
    rim -= Pos(0, 0, -LIP_H / 2) * Box(
        rim_l - 2 * LIP_WALL, rim_w - 2 * LIP_WALL, LIP_H + 0.5
    )
    part += rim

    part -= Pos(0, 4.0, LID_TOP / 2) * Box(CAMERA_WINDOW_L, CAMERA_WINDOW_W, LID_TOP * 4)
    part -= Pos(MIC_POS_X, MIC_POS_Y, LID_TOP / 2) * Cylinder(MIC_R, LID_TOP * 4)

    for x, y in BUTTON_POS:
        part -= Pos(x, y, LID_TOP / 2) * Cylinder(BUTTON_R, LID_TOP * 4)
    return part


def main():
    OUT.mkdir(exist_ok=True)
    parts = {
        "open_plaud_xiao_lipo_base.stl": base_tray(),
        "open_plaud_xiao_lipo_lid.stl": lid(),
    }
    for name, solid in parts.items():
        path = OUT / name
        assert solid.volume > 0, f"{name}: empty solid — check parameters"
        export_stl(solid, str(path), ascii_format=True)
        bb = solid.bounding_box()
        size = bb.size
        print(
            f"wrote {path.name:34s} {path.stat().st_size:>8d} B  "
            f"bbox {size.X:.1f}×{size.Y:.1f}×{size.Z:.1f} mm"
        )
    print(
        f"assembled outer footprint ~ {OUTER_L:.1f} × {OUTER_W:.1f} × "
        f"{BASE_H + LID_TOP:.1f} mm"
    )


if __name__ == "__main__":
    main()
