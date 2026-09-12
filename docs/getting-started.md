# Getting Started

Use this guide to connect an ESP32 board from this repository to an existing OpenClaw gateway.

## What You Need

- OpenClaw installed and `openclaw` available on `PATH`
- An OpenClaw gateway the board can reach
- The ESP-IDF revision and patch set required by your chosen example; follow
  the [Tab5 SDK compatibility procedure](../examples/m5stack-tab5-room-node/README.md)
  rather than selecting an unmodified release tag
- An ESP32 board with Wi-Fi
- A serial connection or board-specific flashing path, depending on the example

Commands below assume the default OpenClaw install. If you use a named profile, add `--profile <profile>` to the `openclaw` commands.

## Choose An Example

- [ESP32 Wi-Fi Node Example](../examples/esp32-node/README.md): Generic ESP32 node with Wi-Fi, GPIO, and ADC commands.
- [ESP-BOX-3 Display Example](../examples/esp-box-3-display/README.md): ESP-BOX-3 node with Wi-Fi and display commands for the built-in screen.
- [Waveshare AMOLED Room Node](../examples/waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md): ESP32-S3 room client with Talk, ambient wake, and an A2UI/image Canvas.
- [M5Stack Tab5 Room Node](../examples/m5stack-tab5-room-node/README.md): ESP32-P4 room client with C6 remote Wi-Fi, Talk, Canvas, camera, and hardware status.

The [historical room-node source-build record](../components/esp-openclaw-room-node/tests/README.md#source-only-firmware-build)
reports ESP-IDF `5.5.5`. The [Tab5 manifest](../examples/m5stack-tab5-room-node/main/idf_component.yml)
still requires that version value, but the current compatibility procedure also
requires SDK source `362a1776ec212788fda95f75b733bfdde3a0c394` and its tracked
patches. Tab5 CI pins the qualified container image and records the actual SDK
and patch identities in firmware provenance; this is not pristine release
`5.5.5`. Other examples retain their own dependency requirements. An accepted
SDK version or successful build does not establish hardware qualification.

## Choose A Delivery Source

These example instructions describe a repository checkout. Room examples use
local component overrides and pinned submodules; initialize the submodules
before building:

```sh
git submodule update --init --recursive
```

Registry packages and prebuilt firmware are separate delivery sources. Record
the resolved package version and archive digest, or the firmware image digest
and associated source commit, rather than assuming they match the checkout.
As of September 7, 2026, the inspected registry `1.0.0` archive referenced
`057a96`, not repository source `3294af3`; the identical component version
string did not imply identical APIs or room-node support. Use the documentation
shipped with the selected artifact, and verify its provenance before applying
current-source instructions.

## Prepare The Gateway

If the board will connect over Wi-Fi to a gateway running on another machine, set `gateway.bind` to `lan` first. The default loopback bind is only reachable from the gateway host itself.

Before pairing a board, set `gateway.nodes.commands.allow` for the example you are using. Each example README lists the commands to allow.

```bash
openclaw config set gateway.bind lan
openclaw config set gateway.nodes.commands.allow '<json-array-from-example>' --strict-json
openclaw gateway restart
openclaw gateway status --probe --json
```

If the gateway stays on loopback, the board cannot reach it over Wi-Fi. If the gateway does not allow the example's commands, the node can connect and still show `commands: []`.

## Build And Flash

Use the commands from the example README for the board you are using. The general flow is:

```bash
. ~/esp-idf/export.sh
cd /path/to/example
idf.py set-target <target>
idf.py build
idf.py -p <serial-port> flash monitor
```

## Pair The Board With The Gateway

Use a setup code for the first connection, or one of the other explicit auth
commands described below.

Generate one on the gateway host:

```bash
openclaw qr \
  --url ws://<gateway-host-ip>:<gateway-port> \
  --setup-code-only
```

The command prints a single setup code:

```text
<setup-code>
```

Sample setup code:

```text
eyJ1cmwiOiJ3czovLzE5Mi4xNjguMS4xMDoxODc4OSIsImJvb3RzdHJhcFRva2VuIjoib2NfYm9vdHN0cmFwX2V4YW1wbGVfdG9rZW4ifQ
```

If the installed gateway already resolves the correct LAN URL, you can omit `--url`. 

After flashing, open the serial console for the board and use the REPL:

```text
openclaw> status
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

Each example README calls out the console path for that board and the commands it exposes after pairing.

- Use `wifi set <ssid>` for an open network, or `wifi set <ssid> <passphrase>` for a secured network.
- The setup code contains a short-lived `bootstrapToken`, not the gateway's
shared token. `gateway setup-code <setup-code>` requests one explicit connect  
attempt.
- If Wi-Fi is still coming up, the REPL waits for the board to obtain an  
IP before it submits that attempt.
- After a successful `hello-ok`, the node
stores the issued `{ gateway_uri, device_token }` reconnect session and uses it
on later `gateway connect` attempts.

Other first-connect options from the REPL are:

- `gateway token <ws://host:port> <token>`
- `gateway password <ws://host:port> <password>`
- `gateway no-auth <ws://host:port>`

`gateway connect` is only for reconnecting with a saved session that already
exists on the board.

## Check The Node From The Gateway

```bash
openclaw nodes status --json
openclaw nodes invoke --node <node-id> --command device.info --json
```

Then run one of the commands from the README for the board you are using.

For room nodes, complete the [hardware qualification checklist](../components/esp-openclaw-room-node/tests/README.md#hardware-qualification)
before treating a successful build or connection as a qualified device.

If pairing did not complete as expected, use [Troubleshooting](./troubleshooting.md).

## Troubleshooting And Reference

- [Troubleshooting](./troubleshooting.md)
- [Component README](../components/esp-openclaw-node/README.md)
