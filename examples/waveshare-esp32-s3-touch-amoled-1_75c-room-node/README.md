# Waveshare ESP32-S3 Touch AMOLED 1.75C room node (WIP)

**Status: experimental / not hardware-qualified.**

This profile is a thin adapter for the **1.75-inch, 466×466** Waveshare
ESP32-S3 Touch AMOLED board. It is not yet a fully supported room node.

Current measured behaviour (face off, `WAVESHARE_DRAW_ROWS=8`, audio stubbed):

- Node session connects and completes the gateway handshake.
- Operator session also connects when the audio path is stubbed.
- Real audio (ES8311/ES7210 + I2S + AFE) plus both WebSocket clients does
  **not** fit in the remaining contiguous internal RAM.
- Animated face (`animation_frame_ms = 16`) monopolizes CPU 0 and trips the
  task watchdog.
- **Talk / WakeNet are not demonstrated.**

See [FINDINGS.md](FINDINGS.md) for the staged heap tables, the face on/off
matrix, and the audio-stub A/B results.

Do not treat a successful flash as proof of full room-node support. The wrong
image for a different Waveshare AMOLED board can fail during display or touch
setup.

## Build and pair

Use ESP-IDF 5.5.5 and initialize the repository's submodules before building.
Back up the device before replacing factory firmware or its partition table.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <serial-port> flash monitor
```

Generate a voice-node setup code on a reachable Gateway:

```sh
openclaw qr --url ws://<gateway-host>:<gateway-port> --voice-node --setup-code-only
```

Use the USB console to provision the board:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

Follow [Getting Started](../../docs/getting-started.md) for Gateway reachability
and command allowlists. Canvas coordinates use this board's **466×466**
dimensions.

## Board behaviour (current WIP)

- Display: CO5300 QSPI via the 1.75C BSP / `esp_lvgl_adapter`.
  `bsp_display_lock()` returns `esp_err_t` (not `bool`). Two partial DMA
  draw buffers of `WAVESHARE_DRAW_ROWS` (default 8) rows are allocated in
  internal DMA-capable memory because the panel cannot DMA from PSRAM.
  (`CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT` is the BSP's own LVGL buffer height
  and is independent of `WAVESHARE_DRAW_ROWS`.)
- Face: disabled by default. Enabling it at 16 ms starves `IDLE0`.
- Audio: gated by `WAVESHARE_AUDIO_STUB` (default 1). The stub leaves the
  codec handles NULL and returns an error so the room node takes the safe
  canvas-only fallback. The real `waveshare_audio_open` (ES8311 playback +
  ES7210 capture) is present but not used by default. Real audio + dual
  clients does not fit the internal RAM budget. Talk / WakeNet are not
  demonstrated.
- Touch and BOOT button use the shared room-node input path.

The [shared room component](../../components/esp-openclaw-room-node/README.md)
owns face animation, Talk, Canvas, session behaviour, and diagnostics. This
example only supplies board wiring. Dual node+operator+audio and live Talk
remain under qualification.

## Qualification status

| Feature                         | Status on 1.75C (this adapter) |
|---------------------------------|--------------------------------|
| Display + touch                 | Works                          |
| Node session                    | Works (face off)               |
| Operator session                | Works only with audio stubbed  |
| Real audio + dual clients       | Does not fit internal RAM      |
| Animated face                   | Watchdog (CPU 0 monopolized)   |
| Live Talk / WakeNet             | Not demonstrated               |

A first upstream slice can reasonably be: 1.75C adapter + face off +
`DRAW_ROWS=8` + documented heap ceiling. Full dual-session audio remains
out of scope until the internal-RAM budget improves.

The audio stub is intentionally the default for this WIP. A future Kconfig
(`CONFIG_WAVESHARE_1_75C_AUDIO_STUB`) would make the opt-in even clearer.
