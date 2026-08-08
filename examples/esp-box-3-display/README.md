# ESP-BOX-3 Display Example

This example runs an ESP-BOX-3 as an OpenClaw Node with display commands for the built-in screen.

<a href="../../docs/assets/openclaw-gateway-esp-box-3-message-flow.png">
  <img src="../../docs/assets/openclaw-gateway-esp-box-3-message-flow.png" alt="OpenClaw Gateway to ESP-BOX-3 message flow" width="900">
</a>

Commands below assume the default OpenClaw install. If you use a named profile, add `--profile <profile>` to the `openclaw` commands.

## What This Example Exposes

- `device`
- `wifi`
- `display`

Commands:

- `device.info`
- `device.status`
- `wifi.status`
- `display.show`
- `display.status`

## Display Payload

`display.show` accepts:

- `heading` short title text, up to `64` UTF-8 bytes
- `text` body text, up to `512` UTF-8 bytes

Sample payload:

```json
{
  "heading": "Hello",
  "text": "OpenClaw is driving the ESP-BOX-3 display."
}
```

On boot, the screen shows a waiting message until the node is paired.

## Prepare The Gateway

If the board will connect over Wi-Fi to a gateway running on another machine, set `gateway.bind` to `lan` first. The default loopback bind is only reachable from the gateway host itself.

Allow this example's commands before pairing the board:

Warning: this command replaces the existing `gateway.nodes.commands.allow` value in the active profile.

```bash
openclaw config set gateway.bind lan
openclaw config set gateway.nodes.commands.allow '[
  "device.info",
  "device.status",
  "wifi.status",
  "display.show",
  "display.status"
]' --strict-json

openclaw gateway restart
openclaw gateway status --probe --json
```

These steps start from an existing OpenClaw gateway that the board can reach on your LAN.

## Build

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp-box-3-display
idf.py set-target esp32s3
idf.py build
```

## Flash

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp-box-3-display
idf.py -p <serial-port> flash monitor
```

## Main REPL Commands

After boot, the example starts the same serial REPL used by the generic ESP32
node example. On the ESP-BOX-3 it is exposed over the native USB Serial/JTAG
console.

The example automatically requests saved-session reconnect after Wi-Fi obtains
an IP and after ordinary connection-loss events. If no saved reconnect session
exists yet, those reconnect attempts are skipped and the board waits for an
explicit gateway auth command.

Start with these commands:

- `status` print saved-session availability and Wi-Fi state
- `wifi set <ssid> [passphrase]` store Wi-Fi credentials in NVS and connect immediately
  - Use `wifi set <ssid>` for an open network.
  - Use `wifi set <ssid> <passphrase>` for a secured network.
- `gateway setup-code <setup-code>` request one setup-code connect attempt; if Wi-Fi is still coming up, the REPL waits for an IP first
- `gateway token <uri> <token>` request one explicit shared-token connect attempt
- `gateway password <uri> <password>` request one explicit password connect attempt
- `gateway no-auth <uri>` request one explicit no-auth connect attempt
- `gateway connect` request one reconnect attempt using the saved reconnect session immediately
- `gateway disconnect` request disconnect of the active session
- `reboot` reboot the board immediately

`status` prints these fields:

- `saved session available`: whether a persisted `{ gateway_uri, device_token }` reconnect session is stored
- `wifi configured`: whether Wi-Fi credentials are saved in NVS
- `wifi ssid`: the saved SSID, when Wi-Fi is configured
- `wifi connected`: whether the board currently has a Wi-Fi connection
- `wifi disconnect reason`: the most recent ESP-IDF disconnect reason, when Wi-Fi is not connected
- `wifi ip`: the current IPv4 address, when Wi-Fi is connected

## First Connection

Generate a setup code on the gateway host:

```bash
openclaw qr \
  --url ws://<gateway-host-ip>:<gateway-port> \
  --setup-code-only
```

The setup code contains a short-lived `bootstrapToken`, not the gateway's shared token.

Bring the board online from the serial REPL:

```text
openclaw> status
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

`gateway setup-code <setup-code>` already requests the connection attempt. If
Wi-Fi is still associating, the REPL waits for an IP before it submits that
attempt. Once a saved reconnect session exists, the example retries it
automatically after Wi-Fi or gateway interruptions. Use `gateway connect` when
you want to trigger that saved-session reconnect immediately.

Then verify the node from the gateway host:

```bash
openclaw nodes status --json
openclaw nodes invoke --node <node-id> --command device.info --json
openclaw nodes invoke --node <node-id> --command display.status --json
```

If pairing did not complete as expected, use [Troubleshooting](../../docs/troubleshooting.md).

## Use The Node

Get basic information:

```bash
openclaw nodes invoke --node <node-id> --command device.info --json
openclaw nodes invoke --node <node-id> --command device.status --json
openclaw nodes invoke --node <node-id> --command wifi.status --json
openclaw nodes invoke --node <node-id> --command display.status --json
```

Update the display:

```bash
openclaw nodes invoke \
  --node <node-id> \
  --command display.show \
  --params '{"heading":"Hello","text":"OpenClaw is driving the ESP-BOX-3 display."}' \
  --json
```

## Other CLI Commands

Useful when you want to test more than the standard setup-code flow:

- `wifi clear`
- `wifi connect`
- `wifi disconnect`
- `reboot`

- `gateway setup-code <code>`
- `gateway no-auth <ws://host:port>`
- `gateway token <ws://host:port> <token>`
- `gateway password <ws://host:port> <password>`
- `gateway connect`
- `gateway disconnect`

<details>
<summary>Gateway command behavior</summary>

- `gateway setup-code ...` performs one explicit setup-code connection attempt
  after Wi-Fi is online
- `gateway no-auth ...` requests one explicit no-auth connect attempt
- `gateway token ...` requests one explicit shared-token connect attempt
- `gateway password ...` requests one explicit password connect attempt
- `gateway connect` performs one reconnect attempt with the saved reconnect session
- `gateway disconnect` is valid only while the session is connected
- This example automatically retries the saved reconnect session after connection loss once Wi-Fi is back

</details>

## Troubleshooting And Reference

- [Troubleshooting](../../docs/troubleshooting.md)
- [Component README](../../components/esp-openclaw-node/README.md)

## Notes

- The `esp32s3` target is fixed for this example.
- The node display name is `OpenClaw ESP-BOX-3`.
