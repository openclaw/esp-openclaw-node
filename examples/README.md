# Examples

This directory contains thin board applications. Reusable provisioning and room-node policy live under `components/`.

## Available Examples

- [ESP32 Wi-Fi Node Example](./esp32-node/README.md) A general-purpose ESP32 node with `device.*`, `wifi.status`, `gpio.*`, and `adc.read`.
- [ESP-BOX-3 Display Example](./esp-box-3-display/README.md) An ESP-BOX-3 node with the shared device and Wi-Fi commands plus `display.show` and `display.status`.
- [Waveshare AMOLED Room Node](./waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md) An always-on room client with WakeNet, device AEC, WebRTC Talk, an A2UI/image Canvas, separate node/operator sessions, and an AMOLED-off idle state.
- [M5Stack Tab5 Room Node](./m5stack-tab5-room-node/README.md) An ESP32-P4 room client using the Tab5 display revisions, C6 remote Wi-Fi, four-slot TDM audio, camera, sensors, SD, and USB-host/RS-485 status.

## CI Firmware Downloads

Successful example jobs in the repository's **Actions > CI** runs upload a
`firmware-<example>-<target>-<event-sha>-<attempt>` artifact. Download and extract
the artifact for your exact board. These are CI builds, not releases or
hardware-qualified images. The generic `esp32-node` CI image targets ESP32-S3;
the Unity test application is not distributed as device firmware.

Each bundle includes all images from ESP-IDF's flash map, including model
partitions, relocated flash arguments, `FLASHING.md`, `manifest.json`, and
`SHA256SUMS`. No source checkout or ESP-IDF installation is needed to flash.
Install the exact esptool version shown in `FLASHING.md` in a Python virtual
environment and use its command with an explicitly selected serial port.
Verify the extracted files before flashing:

```sh
# Linux
sha256sum -c SHA256SUMS
# macOS
shasum -a 256 -c SHA256SUMS
```

The manifest records the actual checked-out source commit separately from the
GitHub event SHA and PR head SHA. A PR build can be a test merge, not the PR head.
It also records the actual IDF commit/build revision, requested Docker image,
esptool and component-manager versions, submodule commits, and payload hashes.
Selected defaults (including target-specific companions) and the selected custom
or IDF built-in partition CSV are identified by normalized paths and SHA-256
hashes. Their source contents are not duplicated into the bundle; the referenced
repository/IDF commits identify them.
Tab5 includes the upstream BSP archive pin/hash and the repository-local bridge
identity; it is not described as an unmodified upstream BSP component.

Sanitized configuration, resolved dependency lock, and build metadata are
provenance records, **not exact rebuild inputs**. The manifest also hashes the
original generated inputs, which packaging leaves untouched. Floating IDF
image tags and dependency ranges still prevent an exact-rebuild guarantee.
Checksums detect corruption; they do not independently authenticate a build.
Only use artifacts from a trusted source commit and workflow run.

Public artifacts use CI defaults. Packaging rejects known credential-bearing
configuration and secure-boot/encrypted builds. Metadata sanitization cannot
remove secrets already compiled into a binary and is not a binary secret scan.
Provision Wi-Fi and the Gateway through the board's serial console after
flashing. Do not add `--force` or erase-all; back up device data before changing
firmware or partition layouts.

The Tab5 bundle flashes the **P4 only**. Its C6 must already run compatible
`esp_hosted` 1.4.0 / `esp_wifi_remote` 0.8.5 firmware, as described in the
[Tab5 prerequisites](./m5stack-tab5-room-node/README.md#c6-wi-fi-prerequisite).
Artifacts expire according to repository retention settings; this workflow does
not publish releases or deploy Pages.

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
