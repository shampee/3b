#!/usr/bin/env python3
"""Regenerates hero_placeholder.png, the 2D demo's character spritesheet.

The sheet this replaced was a third-party asset of unclear licensing, so
this draws its own: a deliberately simple flat-shaded figure, checked in
alongside the generator that made it so the asset is unambiguously part
of this repository rather than something vendored.

The LAYOUT is what examples/game/character.3b actually depends on, and
must not drift from it: a 10-column x 9-row grid of 128x128 cells, one
animation per row, frames packed left-to-right from column 0. Row order
and frame counts are ROWS below; character.3b's make-row-clip calls
mirror them, so change both together.

    python3 make_placeholder_sheet.py     # writes hero_placeholder.png

Needs Pillow. Nothing in the build runs this -- the PNG it produces is
committed, and this is here for whoever wants to change the art.
"""

import math
from PIL import Image, ImageDraw

CELL = 128
COLS, ROWS_N = 10, 9

# (name, frame count) -- index is the row, and must match character.3b.
ROWS = [
    ("standing", 1),
    ("idling", 10),
    ("walking", 10),
    ("running", 10),
    ("jumping", 6),
    ("falling", 4),
    ("falling-loop", 3),
    ("sword-sweep", 3),
    ("sweep-end", 4),
]

SKIN = (232, 186, 152, 255)
TUNIC = (68, 116, 176, 255)
BELT = (40, 62, 96, 255)
LEGS = (74, 82, 102, 255)
BOOT = (44, 40, 52, 255)
# The far-side limb of each pair, a shade darker, so a swing still reads as
# two limbs when the near one crosses in front of it.
TUNIC_FAR = (50, 88, 138, 255)
LEGS_FAR = (56, 63, 80, 255)
HAIR = (96, 66, 48, 255)
BLADE = (206, 212, 222, 255)
HILT = (150, 108, 56, 255)


def limb(d, a, b, colour, width):
    d.line([a, b], fill=colour, width=width)
    r = width // 2
    for p in (a, b):
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=colour)


def draw_figure(d, cx, base_y, *, lean=0.0, bob=0.0,
                arm_l=0.0, arm_r=0.0, leg_l=0.0, leg_r=0.0, sword=None):
    """One figure, centred on cx with its feet at base_y.

    Angles are radians measured from straight down, positive swinging
    forward (+x). `lean` shifts the upper body forward, `bob` lifts the
    whole figure.
    """
    base_y -= bob
    hip = (cx + lean * 0.5, base_y - 42)
    shoulder = (cx + lean, base_y - 74)
    head_c = (cx + lean * 1.2, base_y - 88)

    # Legs and arms hang from OFFSET anchors, not from a single point: two
    # limbs swinging by equal-and-opposite small angles land on top of each
    # other otherwise, and the figure reads as one-legged at rest.
    for ang, dx, shade in ((leg_l, -6, LEGS), (leg_r, 6, LEGS_FAR)):
        root = (hip[0] + dx, hip[1])
        knee = (root[0] + math.sin(ang) * 12, root[1] + math.cos(ang) * 21)
        foot = (root[0] + math.sin(ang) * 24, root[1] + math.cos(ang) * 40)
        limb(d, root, knee, shade, 11)
        limb(d, knee, foot, shade, 10)
        d.ellipse([foot[0] - 7, foot[1] - 4, foot[0] + 7, foot[1] + 4], fill=BOOT)

    # neck, then torso over the leg roots
    d.rectangle([head_c[0] - 5, head_c[1] + 8, head_c[0] + 5, shoulder[1] + 3], fill=SKIN)
    d.polygon([(shoulder[0] - 15, shoulder[1]), (shoulder[0] + 15, shoulder[1]),
               (hip[0] + 13, hip[1] + 3), (hip[0] - 13, hip[1] + 3)], fill=TUNIC)
    d.rectangle([hip[0] - 14, hip[1] - 5, hip[0] + 14, hip[1] + 3], fill=BELT)

    hands = []
    for ang, dx, shade in ((arm_l, -13, TUNIC_FAR), (arm_r, 13, TUNIC)):
        root = (shoulder[0] + dx, shoulder[1] + 4)
        elbow = (root[0] + math.sin(ang) * 15, root[1] + math.cos(ang) * 15)
        hand = (root[0] + math.sin(ang) * 29, root[1] + math.cos(ang) * 29)
        limb(d, root, elbow, shade, 9)
        limb(d, elbow, hand, SKIN, 8)
        hands.append(hand)

    # head
    d.ellipse([head_c[0] - 14, head_c[1] - 15, head_c[0] + 14, head_c[1] + 15], fill=SKIN)
    d.chord([head_c[0] - 14, head_c[1] - 16, head_c[0] + 14, head_c[1] + 10],
            180, 360, fill=HAIR)

    if sword is not None:
        grip = hands[1]
        tip = (grip[0] + math.sin(sword) * 46, grip[1] + math.cos(sword) * 46)
        limb(d, grip, tip, BLADE, 6)
        d.ellipse([grip[0] - 6, grip[1] - 6, grip[0] + 6, grip[1] + 6], fill=HILT)


def pose(row, i, n):
    """Per-row pose parameters for frame i of n."""
    t = i / max(n - 1, 1)
    ph = 2 * math.pi * i / n
    if row == 0:                                    # standing
        return dict(arm_l=0.18, arm_r=-0.18, leg_l=0.08, leg_r=-0.08)
    if row == 1:                                    # idling
        return dict(bob=1.5 * math.sin(ph), arm_l=0.2 + 0.08 * math.sin(ph),
                    arm_r=-0.2 - 0.08 * math.sin(ph), leg_l=0.08, leg_r=-0.08)
    if row == 2:                                    # walking
        s = math.sin(ph)
        return dict(bob=1.5 * abs(math.cos(ph)), lean=1.0,
                    arm_l=0.5 * s, arm_r=-0.5 * s, leg_l=-0.5 * s, leg_r=0.5 * s)
    if row == 3:                                    # running
        s = math.sin(ph)
        return dict(bob=3.0 * abs(math.cos(ph)), lean=6.0,
                    arm_l=0.9 * s, arm_r=-0.9 * s, leg_l=-0.9 * s, leg_r=0.9 * s)
    if row == 4:                                    # jumping (one-shot)
        return dict(bob=16 * math.sin(math.pi * t * 0.75), lean=2.0,
                    arm_l=2.4 * t, arm_r=2.4 * t,
                    leg_l=-0.7 * t, leg_r=0.7 * t)
    if row == 5:                                    # falling (one-shot)
        return dict(bob=6 * (1 - t), lean=-2.0, arm_l=2.6, arm_r=2.6,
                    leg_l=-0.5 - 0.3 * t, leg_r=0.5 + 0.3 * t)
    if row == 6:                                    # falling-loop
        s = math.sin(ph)
        return dict(lean=-2.0, arm_l=2.6 + 0.15 * s, arm_r=2.6 - 0.15 * s,
                    leg_l=-0.6, leg_r=0.6)
    if row == 7:                                    # sword-sweep (one-shot)
        return dict(lean=3.0 * t, arm_l=0.3, arm_r=-2.2 + 3.4 * t,
                    leg_l=0.3, leg_r=-0.3, sword=-2.6 + 4.0 * t)
    return dict(lean=3.0 * (1 - t), arm_l=0.25, arm_r=1.2 - 1.4 * t,
                leg_l=0.2, leg_r=-0.2, sword=1.4 - 1.6 * t)   # sweep-end


def main():
    sheet = Image.new("RGBA", (COLS * CELL, ROWS_N * CELL), (0, 0, 0, 0))
    for row, (_name, count) in enumerate(ROWS):
        for i in range(count):
            cell = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
            draw_figure(ImageDraw.Draw(cell), CELL // 2, CELL - 14, **pose(row, i, count))
            sheet.paste(cell, (i * CELL, row * CELL))
    sheet.save("hero_placeholder.png")
    print("wrote hero_placeholder.png  %dx%d" % sheet.size)


if __name__ == "__main__":
    main()
