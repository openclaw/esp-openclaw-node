# Examples

This directory contains the example applications in this repository and the shared source they build on.

## Available Examples

- [ESP32 Wi-Fi Node Example](./esp32-node/README.md) A general-purpose ESP32 node with `device.*`, `wifi.status`, `gpio.*`, and `adc.read`.
- [ESP-BOX-3 Display Example](./esp-box-3-display/README.md) An ESP-BOX-3 node with the shared device and Wi-Fi commands plus `display.show` and `display.status`.
- [Waveshare AMOLED Room Node](./waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md) An always-on room client with WakeNet, device AEC, WebRTC Talk, an A2UI/image Canvas, separate node/operator sessions, and an AMOLED-off idle state.

## Directory Structure

- `common/` Shared source used by more than one example. This is not a standalone example.
- `esp32-node/` The generic ESP32 example.
- `esp-box-3-display/` The ESP-BOX-3 example.
- `waveshare-esp32-s3-touch-amoled-2.06-room-node/` The ambient voice-room example.

## Naming Convention

- `*_node_cmd.c` OpenClaw Node command handlers and the function that registers those commands with the node.
- `*_repl_cmd.c` REPL command handlers and the function that registers those commands with the console.
- Other `.c` files Helper code, board setup, runtime services, or the main application entry point.

The public `esp_openclaw_node` API passes command parameters as raw JSON text. The examples parse that text with `cJSON` explicitly inside the example sources.

## Common Directory

The shared files under `common/` are split the same way:

- Shared device and Wi-Fi node commands: [esp_openclaw_node_common_device_node_cmd.c](./common/esp_openclaw_node_common_device_node_cmd.c)
- Shared JSON parsing and payload helpers: [esp_openclaw_node_example_json.c](./common/esp_openclaw_node_example_json.c)
- Shared REPL startup: [esp_openclaw_node_example_repl.c](./common/esp_openclaw_node_example_repl.c)
- Shared REPL commands such as `status`, `wifi`, `gateway`, and `reboot`: [esp_openclaw_node_example_repl_cmd.c](./common/esp_openclaw_node_example_repl_cmd.c)
- Shared saved-session reconnect helper: [esp_openclaw_node_example_saved_session_reconnect.c](./common/esp_openclaw_node_example_saved_session_reconnect.c)
- Shared Wi-Fi helpers: [esp_openclaw_node_wifi.c](./common/esp_openclaw_node_wifi.c)
