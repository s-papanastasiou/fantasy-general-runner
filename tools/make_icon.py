#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw the runner's icon (an original design: a shield with crossed swords)
and write runner/win/fg-runner.ico and runner/linux/fg-runner.png.

    python3 tools/make_icon.py      (needs Pillow)
"""
import math
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 1024  # drawn large, then scaled down for smooth edges


def shield(cx, top, w, h):
    """A heater shield: straight top, sides curving to a point."""
    pts = []
    for i in range(41):
        t = i / 40
        x = cx + w / 2 * (1 - t ** 2.2)
        y = top + h * 0.35 + (h * 0.65) * t
        pts.append((x, y))
    right = [(cx + w / 2, top)] + pts
    left = [(2 * cx - x, y) for x, y in reversed(right)]
    return right + left


def sword(draw, angle, colour, guard, length=620, width=56):
    c, s = math.cos(angle), math.sin(angle)

    def at(u, v):  # u along the blade from the centre, v across it
        return (S / 2 + u * c - v * s, S / 2 - 10 + u * s + v * c)

    tip, base = length / 2, -length / 2
    draw.polygon([at(tip, 0), at(tip - 90, width / 2), at(base + 150, width / 2),
                  at(base + 150, -width / 2), at(tip - 90, -width / 2)], fill=colour)
    draw.polygon([at(base + 150, 120), at(base + 118, 120), at(base + 118, -120), at(base + 150, -120)], fill=guard)
    draw.polygon([at(base + 118, 22), at(base + 25, 22), at(base + 25, -22), at(base + 118, -22)], fill=(90, 50, 25))
    p = at(base + 10, 0)
    draw.ellipse([p[0] - 32, p[1] - 32, p[0] + 32, p[1] + 32], fill=guard)


def draw_icon():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.polygon(shield(S / 2, 60, 860, 900), fill=(212, 166, 52))        # gold rim
    d.polygon(shield(S / 2, 110, 760, 800), fill=(28, 52, 120))        # blue field
    sword(d, math.radians(-50), (225, 228, 235), (212, 166, 52))
    sword(d, math.radians(-130), (205, 210, 220), (212, 166, 52))
    return img


def main():
    img = draw_icon()
    win = os.path.join(ROOT, "runner", "win", "fg-runner.ico")
    lin = os.path.join(ROOT, "runner", "linux", "fg-runner.png")
    os.makedirs(os.path.dirname(lin), exist_ok=True)
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.resize((256, 256), Image.LANCZOS).save(win, sizes=sizes)
    img.resize((256, 256), Image.LANCZOS).save(lin, optimize=True)
    print("wrote", win, "and", lin)


if __name__ == "__main__":
    sys.exit(main())
