#!/usr/bin/env python3
"""Bake the mobile mascot's independently animated parts for LVGL.

Run with: python scripts/generate_watch_lobster.py --output-dir OUTPUT
Source geometry: OpenClawMascotView.swift / favicon.svg, see assets/NOTICE.md.
Uses only the Python standard library already required by ESP-IDF.
"""

import argparse
import math
from pathlib import Path


SCALE = 1.2
SUPERSAMPLE = 4
FRAME_SIZE = 196
# Crops include transparent padding for antialiasing and the native hinge positions.
PARTS = [
    ("body", (14, 9, 106, 111), [
        ("M", 60, 10), ("C", 30, 10, 15, 35, 15, 55),
        ("C", 15, 75, 30, 95, 45, 100), ("L", 45, 110), ("L", 55, 110),
        ("L", 55, 100), ("C", 55, 100, 60, 102, 65, 100),
        ("L", 65, 110), ("L", 75, 110), ("L", 75, 100),
        ("C", 90, 95, 105, 75, 105, 55), ("C", 105, 35, 90, 10, 60, 10), ("Z",),
    ], (15, 10, 105, 110)),
    ("left_claw", (2, 42, 28, 67), [
        ("M", 20, 45), ("C", 5, 40, 0, 50, 5, 60),
        ("C", 10, 70, 20, 65, 25, 55), ("C", 28, 48, 25, 45, 20, 45), ("Z",),
    ], (3.125, 43.67, 26.197, 65.451)),
    ("right_claw", (92, 42, 118, 67), [
        ("M", 100, 45), ("C", 115, 40, 120, 50, 115, 60),
        ("C", 110, 70, 100, 65, 95, 55), ("C", 92, 48, 95, 45, 100, 45), ("Z",),
    ], (93.803, 43.67, 116.875, 65.451)),
    # Quadratic antenna paths converted exactly to cubic Beziers.
    ("left_antenna", (28, 5, 47, 17), [
        ("M", 45, 15), ("C", 38.333333, 8.333333, 33.333333, 6, 30, 8),
    ], None),
    ("right_antenna", (73, 5, 92, 17), [
        ("M", 75, 15), ("C", 81.666667, 8.333333, 86.666667, 6, 90, 8),
    ], None),
]


def points_for(commands):
    points = []
    for command, *coords in commands:
        if command in ("M", "L"):
            points.append(tuple(coords))
        elif command == "C":
            x0, y0 = points[-1]
            x1, y1, x2, y2, x3, y3 = coords
            for i in range(1, 65):
                t = i / 64
                u = 1 - t
                points.append((u**3*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                               u**3*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
    return points


def raster_mask(points, width, height, filled):
    """Scan-convert at 4x resolution, including round antenna caps."""
    mask = bytearray(width * height)
    if filled:
        edges = list(zip(points, points[1:] + points[:1]))
        for y in range(height):
            scan = y + 0.5
            crossings = sorted(x1 + (scan-y1)*(x2-x1)/(y2-y1)
                               for (x1,y1),(x2,y2) in edges
                               if min(y1,y2) <= scan < max(y1,y2))
            for x1, x2 in zip(crossings[::2], crossings[1::2]):
                start, end = max(0, math.ceil(x1-0.5)), min(width, math.ceil(x2-0.5))
                mask[y*width+start:y*width+end] = bytes([255]) * (end-start)
    else:
        radius = 1.5 * SCALE * SUPERSAMPLE
        for (x1,y1),(x2,y2) in zip(points, points[1:]):
            dx, dy = x2-x1, y2-y1
            length = dx*dx + dy*dy
            for y in range(max(0, int(min(y1,y2)-radius)), min(height, math.ceil(max(y1,y2)+radius))):
                for x in range(max(0, int(min(x1,x2)-radius)), min(width, math.ceil(max(x1,x2)+radius))):
                    t = max(0, min(1, ((x+0.5-x1)*dx+(y+0.5-y1)*dy)/length))
                    if (x+0.5-x1-t*dx)**2 + (y+0.5-y1-t*dy)**2 <= radius*radius:
                        mask[y*width+x] = 255
    return mask


def frame_pixel(x, y):
    x -= FRAME_SIZE / 2
    y -= FRAME_SIZE / 2
    radius = math.hypot(x, y)
    if radius <= 87:
        light = max(0, 1 - math.hypot((x + 35) / 120, (y + 65) / 140))
        warmth = max(0, 1 - math.hypot(x / 100, (y - 55) / 90))
        rgb = [27 + 16 * light + 16 * warmth,
               22 + 14 * light + 3 * warmth,
               33 + 16 * light + 8 * warmth]
        rim = max(0, 1 - abs(radius - 86.4)) * (0.16 + 0.25 * max(0, -y / 87))
        rgb = [c * (1 - rim) + 230 * rim for c in rgb]
        shadow = max(0, 1 - (x / 34) ** 2 - ((y - 65) / 6) ** 2) * 0.55
        return (*[round(c * (1 - shadow)) for c in rgb], 255)
    if abs(radius - 97) < 0.5:
        return (192, 175, 190, round(12 + 24 * max(0, -y / 97)))
    return (0, 0, 0, 0)


def frame_pixels():
    pixels = bytearray()
    for y in range(FRAME_SIZE):
        for x in range(FRAME_SIZE):
            samples = [frame_pixel(x + (sx + 0.5) / SUPERSAMPLE,
                                   y + (sy + 0.5) / SUPERSAMPLE)
                       for sy in range(SUPERSAMPLE) for sx in range(SUPERSAMPLE)]
            coverage = sum(p[3] for p in samples)
            if coverage == 0:
                pixels.extend((0, 0, 0, 0))
                continue
            rgb = [round(sum(p[c] * p[3] for p in samples) / coverage) for c in range(3)]
            pixels.extend((*reversed(rgb), round(coverage / len(samples))))
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    assets = parser.parse_args().output_dir
    assets.mkdir(parents=True, exist_ok=True)
    pixels = bytearray()
    entries = []
    for name, crop, commands, gradient in PARTS:
        left, top, right, bottom = [round(v * SCALE) for v in crop]
        width, height = right - left, bottom - top
        points = [((x*SCALE-left)*SUPERSAMPLE, (y*SCALE-top)*SUPERSAMPLE)
                  for x,y in points_for(commands)]
        mask = raster_mask(points, width*SUPERSAMPLE, height*SUPERSAMPLE, gradient is not None)
        image = bytearray()
        for y in range(height):
            for x in range(width):
                t = 0
                if gradient:
                    x1, y1, x2, y2 = gradient
                    dx, dy = x2 - x1, y2 - y1
                    t = max(0, min(1, (((x + left + 0.5) / SCALE - x1) * dx
                                      + ((y + top + 0.5) / SCALE - y1) * dy) / (dx * dx + dy * dy)))
                alpha = round(sum(mask[(y*SUPERSAMPLE+sy)*width*SUPERSAMPLE+x*SUPERSAMPLE+sx]
                                  for sy in range(SUPERSAMPLE) for sx in range(SUPERSAMPLE))
                              / (SUPERSAMPLE*SUPERSAMPLE))
                # Little-endian LVGL ARGB8888 is stored in BGRA order.
                image.extend((round(77-50*t), round(77-50*t), round(255-102*t), alpha))
        entries.append(f"    {{ {len(pixels)}, {left}, {top}, {width}, {height} }}, /* {name} */")
        pixels.extend(image)
    frame_offset = len(pixels)
    pixels.extend(frame_pixels())
    (assets / "watch_lobster.argb8888").write_bytes(pixels)
    (assets / "watch_lobster_parts.h").write_text(
        "/* Generated by scripts/generate_watch_lobster.py; do not edit. */\n"
        + f"#define WATCH_LOBSTER_SIZE {round(120 * SCALE)}\n"
        + f"#define WATCH_LOBSTER_FRAME_SIZE {FRAME_SIZE}\n"
        + f"#define WATCH_LOBSTER_FRAME_OFFSET {frame_offset}\n"
        "static const struct { uint32_t offset; int16_t x, y, width, height; } LOBSTER_PARTS[] = {\n"
        + "\n".join(entries) + "\n};\n"
    )
    print(f"Wrote {len(pixels)} bytes: {len(entries)} animated parts and one static frame")


if __name__ == "__main__":
    main()
