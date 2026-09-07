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
