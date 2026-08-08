# M5Stack Tab5 Room Node

Thin ESP32-P4 board adapter for the canonical `esp-openclaw-room-node`
component, targeting ESP-IDF 5.5.5 and the `espressif/m5stack_tab5` 1.2.0~1
source/API contract.

## Build and provision

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

The official BSP manifest pins `esp_video ~2.0`, while P4-capable
`esp_capture` requires `esp_video ^2.1`. The source-only BSP build bridge uses
exact inspected commit `f0ef9497efce684997ce391edd19733483e250a5` without
patching/copying BSP production source, and resolves maintained esp_video 2.3.
The exact commit tarball is the default. Developers who already have the same
checkout may explicitly set `OPENCLAW_TAB5_BSP_LOCAL_PATH`; configuration
rejects it unless HEAD is that commit and the worktree is clean.

Provision from USB with `wifi set <ssid> <passphrase>` and `gateway setup-code
<code>`. Kconfig credentials only seed an unconfigured unit.

## C6 Wi-Fi prerequisite

The ESP32-C6 must already run firmware compatible with `esp_hosted` 1.4.0 and
`esp_wifi_remote` 0.8.5. The adapter powers `BSP_FEATURE_WIFI` and uses CMD13,
CLK12, D0/1/2/3 11/10/9/8, reset GPIO15 active-low, four-bit 40 MHz SDIO, C6
target. Missing transport is shown as `Wi-Fi coprocessor unavailable`.

## Display and audio

The maintained MIPI-DSI/LVGL stack rotates to 1280x720 landscape and probes
ILI9881C+GT911, ST7123 (touch firmware 3), and ST7121 (firmware 1). ST7123 is
physically verified on the connected unit. ST7121 is compile-tested, not
hardware-proven here, and uses only the isolated Apache-2.0 panel extension
adapted from M5Stack's official `M5Tab5-UserDemo` commit
`68b19d37fbf9cefd5f256992f5dca34794c62ab4`; touch remains on the maintained
ST7123-compatible API. No S3 bounce-buffer workaround is present.

The board adapter keeps the official four-slot TDM order: MIC-L, speaker
reference, MIC-R, headset mic. Shared AEC/WakeNet consume MIC-L plus the
far-end reference; dual-mic BSS is intentionally disabled because it starves
the task watchdog's CPU0 idle subscriber on early 360 MHz P4 silicon. Ambient
capture uses the speech-recognition AFE, while active Talk reopens the source in
16 kHz voice-communication mode with WakeNet and redundant device NS/VAD
disabled; AEC and nonlinear echo suppression remain active. Talk and voiceWake
advertise only after media/wake init. Playback and capture use jointly
allocated, reciprocally paired I2S channels with distinct data lines and the
shared 48 kHz, 64-bit frame contract; the ES7210 input policy is 30 dB.

## Commands and policy

Commands outside the Gateway's generic platform policy require an explicit
`gateway.nodes.commands.allow` entry. This example's full device-owned allow
set is:

```json
["camera.list","camera.snap","canvas.present","canvas.navigate","canvas.hide","canvas.snapshot","canvas.a2ui.pushJSONL","canvas.a2ui.push","canvas.a2ui.reset","dir.list","file.fetch","file.write","hardware.status","face.set","face.gesture","talk.start","talk.stop"]
```

`camera.snap` is privacy-heavy. The firmware refuses to open the camera unless
the UI controller has visibly armed and flushed its capture indicator, and
camera/Talk media ownership is serialized. Tab5 exposes only its front camera:
use `--facing front`. The firmware rejects `back` instead of duplicating or
mislabeling the front image.

```sh
openclaw nodes camera snap --node <tab5-node> --facing front
```

| Surface | Commands and bounds |
| --- | --- |
| Device | `device.info`, `device.status`, `wifi.status` |
| Talk | `talk.start`, `talk.stop` |
| Canvas | `present`, `navigate`, `hide`, `snapshot`, `a2ui.pushJSONL`, compatibility `a2ui.push`, `a2ui.reset`; no `canvas.eval` |
| A2UI action | Fixed `CANVAS_A2UI` agent request; id/name/surface/component <=64, context <=2 KiB |
| Camera | `camera.list`, `camera.snap --facing front`; fixed 1280x720 RGB565 sensor capture, converted by PPA to rotated/downscaled RGB888 before JPEG encoding, <=1 MP and <=1 MiB |
| Files | `dir.list`, `file.fetch`, `file.write`; canonical `/sd/**`, <=1 MiB, strict base64/SHA-256, no-overwrite default |
| Hardware | `hardware.status`, <=8 KiB typed partial BMI270, RX8130CE, INA226, SD, USB, RS-485 sections |

INA226 is power-path telemetry, not a fuel gauge: battery percentage, battery
health, source, and charging are never fabricated.

## GPIO20 mode and limits

`menuconfig -> M5Stack Tab5 room board -> GPIO20 hardware mode` exclusively
selects `USB_HOST` (default) or RS-485 UART1 half-duplex TX20/RX21/DE34. The
other section reports disabled/conflict. USB inventory never exposes serial
numbers and is capped at eight.

Canvas limits remain 96 components, 256 KiB input/store, depth 8, six images,
2 MiB fetch, 2048 px/side, one decoded megapixel. There is no `camera.clip`
(long-media audio ownership/payload/cancellation is unresolved), `/usb/<n>`
until a mass-storage driver mounts it, raw USB/RS-485 tunnel, arbitrary file
root, reboot/shutdown/power mutation, or continuous sensor/media stream.

File preflights are side-effect free. New no-overwrite files use a temp file
and the filesystem's no-replace rename behavior. FAT replacement cannot rename
over an existing target, so overwrite uses a recoverable transaction (old file
to bounded backup, temp to target, restore on failure) rather than claiming a
fully atomic replacement. A 1 MiB decoded write fits below the node's 2 MiB
assembled-message ceiling after base64/JSON overhead; larger writes are not
supported.
