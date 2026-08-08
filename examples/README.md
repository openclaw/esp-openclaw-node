# Examples

This directory contains thin board applications. Reusable provisioning and room-node policy live under `components/`.

## Available Examples

- [ESP32 Wi-Fi Node Example](./esp32-node/README.md) A general-purpose ESP32 node with `device.*`, `wifi.status`, `gpio.*`, and `adc.read`.
- [ESP-BOX-3 Display Example](./esp-box-3-display/README.md) An ESP-BOX-3 node with the shared device and Wi-Fi commands plus `display.show` and `display.status`.
- [Waveshare AMOLED Room Node](./waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md) An always-on room client with WakeNet, device AEC, WebRTC Talk, an A2UI/image Canvas, separate node/operator sessions, and an AMOLED-off idle state.
- [M5Stack Tab5 Room Node](./m5stack-tab5-room-node/README.md) An ESP32-P4 room client using the Tab5 display revisions, C6 remote Wi-Fi, four-slot TDM audio, camera, sensors, SD, and USB-host/RS-485 status.

## Directory Structure

- `esp32-node/` The generic ESP32 example.
- `esp-box-3-display/` The ESP-BOX-3 example.
- `waveshare-esp32-s3-touch-amoled-2.06-room-node/` The ambient voice-room example.
- `m5stack-tab5-room-node/` The ESP32-P4 Tab5 room-node example.

## Naming Convention

- `*_node_cmd.c` OpenClaw Node command handlers and the function that registers those commands with the node.
- `*_repl_cmd.c` REPL command handlers and the function that registers those commands with the console.
- Other `.c` files Helper code, board setup, runtime services, or the main application entry point.

The public `esp_openclaw_node` API passes command parameters as raw JSON text. The examples parse that text with `cJSON` explicitly inside the example sources.

## Shared provisioning

The former `common/` sources now live in the reusable `esp-openclaw-node-provisioning` component:

- device/Wi-Fi commands and JSON validation
- USB REPL and gateway commands
- saved-session reconnect policy
- NVS-backed station Wi-Fi
