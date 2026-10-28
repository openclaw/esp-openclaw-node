# OpenClaw Home Image

`openclaw_lobster.argb8888` is the existing transparent 180 x 180 OpenClaw
`ui/public/apple-touch-icon.png`, converted without resizing to straight-alpha
BGRA bytes for LVGL's little-endian ARGB8888 format. It contains exactly 129600
bytes and is embedded as read-only firmware data. No PNG decoder, image
animation, or per-frame image allocation is used.

Source: https://github.com/openclaw/openclaw/blob/56cd7472a3ce190e866fbe67838fc45705e6b2cd/ui/public/apple-touch-icon.png

License: https://github.com/openclaw/openclaw/blob/56cd7472a3ce190e866fbe67838fc45705e6b2cd/LICENSE

Conversion uses Pillow 11.3.0: `Image.open(source).convert("RGBA").tobytes("raw", "BGRA")`.

SHA-256:

- Source PNG: `e8c7ce0a3a6c52bd904cc55e31a5b3a8b6392dcd70ba9e220ecf0ef1d6bd8c16`
- Embedded bitmap: `984065fcee3b54174a3849c56ba5c31c890ee2bca35124e88ce287255aa58044`

## Watch animation parts

`watch_lobster.argb8888` and `watch_lobster_parts.h` are generated in the build
directory by `scripts/generate_watch_lobster.py` from the body, claw, and antenna
geometry used by the mobile app. The renderer draws the eyes and mouth with
LVGL and reuses the existing face lifecycle, moods, gestures, and speech level.
Five cropped, straight-alpha BGRA images use a 144 x 144 pixel mascot canvas.
A static 196 x 196 pixel image adds the 174 pixel circular frame, highlights,
and ground shadow. All six images total 217,488 bytes of read-only flash data;
no image decoder, runtime blur, or full-mascot scaling is needed. Generation
uses Python's standard library with 4x supersampling and requires no additional
build dependency. The frame stays still while the existing face lifecycle
animates the mascot's parts.

Source geometry and dark palette:

- [OpenClawMascotView.swift](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/apps/shared/OpenClawKit/Sources/OpenClawChatUI/OpenClawMascotView.swift)
- [favicon.svg](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/ui/public/favicon.svg)

The same MIT license below applies to these derived assets.

## MIT License

Copyright (c) 2026 OpenClaw Foundation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
