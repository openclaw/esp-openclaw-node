# Waveshare ESP32-S3 Touch AMOLED 1.8 watch node

This profile runs the shared OpenClaw room node on the **1.8-inch, 368×448**
Waveshare watch board with 16 MB flash and 8 MB PSRAM. It provides authenticated
node/operator sessions, reconnect, microphone capture, WakeNet, WebRTC Talk,
an animated agent face, image/A2UI Canvas, and the shared diagnostics overlay.

Use this profile only for the 1.8-inch board. The 2.06-inch board has different
display, touch, and audio hardware. A successful flash does not identify a
compatible board; the wrong image can enter a boot loop during touch setup.

## Build and pair

Use ESP-IDF 5.5.5 and initialize the repository's submodules before building.
Back up the device before replacing factory firmware or its partition table.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <watch-serial-port> flash monitor
```

Generate a voice-node setup code on a reachable Gateway:

```sh
openclaw qr --url ws://<gateway-host>:<gateway-port> --voice-node --setup-code-only
```

Use the USB console to provision the watch:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

Follow [Getting Started](../../docs/getting-started.md) for Gateway reachability
and command allowlists. This board uses the same room-node commands described
in the [shared command reference](../waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md#canvas-commands).
Canvas coordinates use this board's **368×448** dimensions.

For a live voice check, follow the shared
[Talk verification steps](../waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md#verify-talk).

## Board behavior

The pinned Waveshare BSP 2.0.3 detects CST816S or FT5x06 touch and applies the
CO5300 panel offset required by the CST816S revision. The adapter registers
this QSPI panel through the LVGL SPI path; the BSP's high-level RGB display
helper is incompatible with this controller. Two 8-row internal DMA
buffers keep display transfers bounded and leave room for the AFE worker and
both Gateway connections, while PSRAM holds the larger UI/audio allocations.
Touch and BOOT-button input use the shared room interface; long
press opens Diagnostics.

The adapter also applies the [board's documented expander reset sequence](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/examples/esp-idf/90_axp2101_pmu/components/board_variant/board_variant.c)
to the panel-power, panel-reset, and touch-reset pins before initialization.

The watch has one ES8311 codec serving both microphone and speaker. One codec
instance and one paired I²S bus serve separate capture and playback handles,
so stopping playback cannot power down an active microphone. Capture declares
one microphone (`M`), with no hardware speaker-reference channel. Wake word and
Talk are available, but this profile does **not** claim acoustic echo
cancellation. Start with the default 70% speaker volume and no digital boost.

The [shared room component](../../components/esp-openclaw-room-node/README.md)
owns face animation, Talk, Canvas, session behavior, and diagnostics. This
example supplies board wiring and audio topology. It does not advertise a
camera, SD file surface, or the Dial's rotary controls.

CI builds and packages the watch image. Physical qualification separately
checks touch, display alignment, microphone/speaker behavior, pairing,
reconnect, wake word, and a live Talk session.
