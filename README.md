![esp-openclaw-node banner](docs/assets/esp-openclaw-node-banner.png)

# esp-openclaw-node

This repository contains the `esp-openclaw-node` ESP-IDF component, example applications, and documentation for running ESP32 boards as [OpenClaw Nodes](https://docs.openclaw.ai/nodes).

<a href="docs/assets/openclaw-gateway-esp-box-3-message-flow.png">
  <img src="docs/assets/openclaw-gateway-esp-box-3-message-flow.png" alt="OpenClaw Gateway to ESP-BOX-3 message flow" width="900">
</a>

## Repository Layout

- `components/esp-openclaw-node/`: The `esp-openclaw-node` ESP-IDF component that handles OpenClaw transport, pairing, reconnect, and command dispatch.  
See the [Component README](./components/esp-openclaw-node/README.md) for more details on component internals.
- `components/esp-openclaw-talk/`: Provider-neutral OpenClaw Talk signaling for Espressif's WebRTC stack.
- `components/esp-openclaw-node-provisioning/`: Reusable Wi-Fi, saved-session, and USB REPL provisioning.
- `components/esp-openclaw-room-node/`: Canonical dual-session room-node product behind narrow board ports.
- `examples/`: Example applications for supported boards.
- `docs/`: Getting-started and troubleshooting guides.

## Start Here

<a href="https://openclaw.github.io/esp-openclaw-node/">
  <img alt="Try it with ESP Launchpad" src="https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png" width="220" height="62">
</a>

1. Read [Getting Started](./docs/getting-started.md).
2. Choose an example from [Examples](./examples/README.md).
3. Use [Troubleshooting](./docs/troubleshooting.md) if the node pairs, connects, or advertises commands differently than expected.
