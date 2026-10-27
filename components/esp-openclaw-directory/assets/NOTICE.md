# Agent avatar images

The build generates `agent_avatars.argb8888`, which contains two uncompressed 64 x 64 images, in this order:

| Image | Byte offset | Byte length |
| --- | ---: | ---: |
| Fox | 0 | 16384 |
| Lobster | 16384 | 16384 |

Pixels use straight alpha and BGRA byte order, matching little-endian LVGL
`LV_COLOR_FORMAT_ARGB8888`. Rows run from top to bottom, with a 256-byte stride.
The total file length is 32768 bytes. Transparent pixels outside the circular
backing are zero; edge pixels are antialiased with 4 x 4 supersampling.

Generate a standalone copy from the repository root:

```sh
python3 scripts/generate_agent_avatars.py --output-dir /tmp/agent-avatars
```

Generation uses only Python's standard library and reuses the path flattening
and mask rasterizer in `scripts/generate_watch_lobster.py`. No fonts or external
images are loaded.

## Sources and adaptations

- **Fox:** the authored `kitsune` SVG in
  `docs/design/watch-dial-ux-flows.html`. Its shapes and colors are retained,
  uniformly scaled by 0.94 around the image center to fit the circular backing.
  The backing retains the source's dark brown, with a narrow lighter rim.
  This is local sample artwork, not a fetched profile image or an emoji-font
  glyph. The repository's Apache-2.0 license applies to this authored artwork.
- **Lobster:** the maintained body, claw and antenna paths and shell gradients
  in `scripts/generate_watch_lobster.py`, scaled into a 54 x 54 area with a
  5-pixel offset. Neutral eyes and their teal highlights follow
  `components/esp-openclaw-room-node/room_lobster.c`. The dark burgundy circular
  backing and rim are added for contrast. The image is static.

The lobster derives from OpenClaw's
[OpenClawMascotView.swift](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/apps/shared/OpenClawKit/Sources/OpenClawChatUI/OpenClawMascotView.swift)
and [favicon.svg](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/ui/public/favicon.svg),
as recorded in `components/esp-openclaw-room-node/assets/NOTICE.md`.
The following license applies to that derived artwork.

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
