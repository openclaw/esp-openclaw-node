#!/usr/bin/env python3
"""Generate fox, then lobster, as two 64 x 64 straight-alpha BGRA frames.

Uses the authored flow-explorer fox and the maintained watch mascot paths.
Only Python's standard library is required; see the asset directory's NOTICE.
"""

import argparse
import math
from pathlib import Path
import sys

# Importing the shared rasterizer must not leave generated files in scripts/.
sys.dont_write_bytecode = True
import generate_watch_lobster as rasterizer


SIZE = 64
SAMPLES = rasterizer.SUPERSAMPLE
EDGE = SIZE * SAMPLES
LOBSTER_SCALE = 54 / 120


def circle(x, y, radius):
    return [(x + radius * math.cos(i * math.tau / 128),
             y + radius * math.sin(i * math.tau / 128)) for i in range(128)]


class Frame:
    def __init__(self, backing, rim):
        self.pixels = bytearray(EDGE * EDGE * 4)
        self.paint(circle(32, 32, 31), rim)
        self.paint(circle(32, 32, 30), backing)

    def paint(self, points, color, filled=True):
        mask = rasterizer.raster_mask(
            [(x * SAMPLES, y * SAMPLES) for x, y in points], EDGE, EDGE, filled)
        for index, covered in enumerate(mask):
            if not covered:
                continue
            x, y = index % EDGE, index // EDGE
            rgb = color((x + 0.5) / SAMPLES, (y + 0.5) / SAMPLES) if callable(color) else color
            self.pixels[index * 4:index * 4 + 4] = bytes((*rgb, 255))

    def bgra(self):
        result = bytearray()
        for y in range(SIZE):
            for x in range(SIZE):
                samples = [((y * SAMPLES + dy) * EDGE + x * SAMPLES + dx) * 4
                           for dy in range(SAMPLES) for dx in range(SAMPLES)]
                coverage = sum(self.pixels[p + 3] for p in samples)
                if coverage == 0:
                    result.extend((0, 0, 0, 0))
                    continue
                # Average premultiplied samples, then unpremultiply for LVGL.
                rgb = [round(sum(self.pixels[p + c] * self.pixels[p + 3]
                                 for p in samples) / coverage) for c in range(3)]
                result.extend((rgb[2], rgb[1], rgb[0], round(coverage / len(samples))))
        return result


def fox():
    frame = Frame((64, 48, 35), (104, 80, 60))

    def paint(points, color):
        # Inset the explorer's 64-unit artwork inside the circular backing.
        frame.paint([(32 + (x - 32) * 0.94, 32 + (y - 32) * 0.94)
                     for x, y in points], color)

    paint([(10, 9), (27, 19), (37, 19), (54, 9), (50, 43), (32, 59), (14, 43)],
          (235, 146, 72))
    paint([(14, 31), (32, 41), (50, 31), (45, 46), (32, 59), (19, 46)],
          (246, 230, 197))
    paint([(10, 9), (14, 26), (24, 21)], (36, 30, 27))
    paint([(54, 9), (50, 26), (40, 21)], (36, 30, 27))
    paint(circle(23, 31, 3), (25, 38, 35))
    paint(circle(41, 31, 3), (25, 38, 35))
    paint([(27, 43), (32, 49), (37, 43)], (36, 30, 27))
    return frame.bgra()


def lobster():
    frame = Frame((48, 32, 41), (95, 60, 74))

    def transform(points):
        return [(5 + x * LOBSTER_SCALE, 5 + y * LOBSTER_SCALE) for x, y in points]

    # The shared rasterizer derives its three-unit antenna stroke from SCALE.
    rasterizer.SCALE = LOBSTER_SCALE
    for _, _, commands, gradient in rasterizer.PARTS:
        if gradient is None:
            color = (255, 77, 77)
        else:
            x1, y1, x2, y2 = gradient
            dx, dy = x2 - x1, y2 - y1

            def color(x, y):
                x, y = (x - 5) / LOBSTER_SCALE, (y - 5) / LOBSTER_SCALE
                t = max(0, min(1, ((x - x1) * dx + (y - y1) * dy) / (dx * dx + dy * dy)))
                return (round(255 - 102 * t), round(77 - 50 * t), round(77 - 50 * t))

        frame.paint(transform(rasterizer.points_for(commands)), color, gradient is not None)
    for x in (45, 75):
        frame.paint(transform(circle(x, 35, 6)), (5, 8, 16))
        frame.paint(transform(circle(x + 1, 34, 2)), (0, 229, 204))
    return frame.bgra()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / "agent_avatars.argb8888"
    pixels = fox() + lobster()
    output.write_bytes(pixels)
    print(f"Wrote {len(pixels)} bytes: fox at 0, lobster at {SIZE * SIZE * 4}")


if __name__ == "__main__":
    main()
