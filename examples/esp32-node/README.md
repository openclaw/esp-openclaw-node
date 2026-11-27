# ESP32 Wi-Fi Node Example

This example runs a Wi-Fi-capable ESP32 board as an OpenClaw Node. It uses the shared serial REPL included with the examples in this repository.

You can build it for ESP32 targets with Wi-Fi support. By default, the node metadata comes from the selected `idf.py` target.

Commands below assume the default OpenClaw install. If you use a named profile, add `--profile <profile>` to the `openclaw` commands.

## What This Example Exposes

- `device`
- `wifi`
- `gpio`
- `adc` on ADC-capable targets

Commands:

- `device.info`
- `device.status`
- `wifi.status`
- `gpio.mode`
- `gpio.read`
- `gpio.write`
- `adc.read`

`adc.read` is only registered when the selected target supports ADC.
The M5StickC and T-Embed profiles also expose the `display` capability with
`display.show` and `display.status`.

## Prepare The Gateway

If the board will connect over Wi-Fi to a gateway running on another machine, set `gateway.bind` to `lan` first. The default loopback bind is only reachable from the gateway host itself.

Set the command allowlist before pairing the board. Without it, the node can connect and still show `commands: []`.

Warning: this command replaces the existing `gateway.nodes.commands.allow` value in the active profile.

```bash
openclaw config set gateway.bind lan
openclaw config set gateway.nodes.commands.allow '[
  "device.info",
  "device.status",
  "wifi.status",
  "gpio.mode",
  "gpio.read",
  "gpio.write",
  "adc.read"
]' --strict-json

openclaw gateway restart
openclaw gateway status --probe --json
```

These steps start from an existing OpenClaw gateway that the board can reach on your LAN.

## Build

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp32-node
idf.py set-target <target>
idf.py build
```

### M5StickC and T-Embed

Use the original M5StickC profile for the ESP32-PICO-D4 with its 80×160 display.
It does not cover the StickC Plus, Plus2, or StickS3. The T-Embed profile
targets the ESP32-S3 model with 16 MB flash; it does not cover the T-Embed
CC1101 or CC1101 Plus.

Build each profile with a separate configuration and output directory:

```sh
idf.py -B build-m5stickc -D SDKCONFIG=sdkconfig.m5stickc.local \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.m5stickc" \
  -D IDF_TARGET=esp32 build
idf.py -B build-t-embed -D SDKCONFIG=sdkconfig.t-embed.local \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lilygo-t-embed" \
  -D IDF_TARGET=esp32s3 build
```

The M5StickC uses its USB-to-UART adapter at 115200 baud. The T-Embed
uses native USB Serial/JTAG. Select the matching build directory and serial
port when flashing. Back up the existing flash before installing either image.
The profiles set board identity, flash size, and console transport; provision
Wi-Fi through the REPL after boot. No Wi-Fi or Gateway credentials belong in
the profile files.

Both profiles show Wi-Fi connectivity, the assigned IPv4 address, and Gateway
connectivity on the built-in screen. The M5StickC enables the AXP192 display
power rails and uses its ST7735 panel. The T-Embed enables its board power and
ST7789 backlight. These profiles do not initialize audio, SD, or radio add-ons.
They use the same serial provisioning commands as the generic example.

The compact profiles also accept display messages:

```sh
openclaw nodes invoke --node <node-id> --command display.show \
  --params '{"heading":"OpenClaw ready","text":"Hello from the Gateway!"}' --json
openclaw nodes invoke --node <node-id> --command display.status --json
```

Add `display.show` and `display.status` to the Gateway's existing command
allowlist before pairing. For these boards, start with `device.info`,
`device.status`, `wifi.status`, `display.show`, and `display.status`; GPIO
commands are not needed to operate the display. Preserve existing entries
when updating a shared Gateway.

Messages have a heading of up to 25 characters and a body of up to 100
characters, wrapped into four rows of 25 columns. Body newlines advance to the
next row; input that exceeds four rows is rejected. The built-in font displays
letters in uppercase and supports digits, spaces, and `. , - _ : ! ? / + % ( ) =`.
Unsupported characters are rejected. Wi-Fi and Gateway status stay visible
in the footer. Messages are kept in RAM and reset after reboot.

`display.show` queues the latest message. Its response and `display.status`
include `generation`, `renderedGeneration`, and `rendered`. A message is
confirmed drawn only when `rendered` is true and both generations match; an
accepted message can be replaced by a newer message before it is drawn.
`ready` becomes false if the display transfer fails. `device.info` reports
the selected board name and model identifier.

Board pin references: [M5StickC](https://docs.m5stack.com/en/core/StickC) and
[T-Embed factory pin map](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/examples/factory/pin_config.h).

Existing `sdkconfig` files take precedence over `sdkconfig.defaults`.
`idf.py set-target` regenerates the project-root `sdkconfig` from those defaults.
On a later build, `idf.py build` alone keeps keys already present in `sdkconfig`;
`idf.py fullclean` and a fresh `-B` build directory do not reset it.

After upstream changes to `sdkconfig.defaults`:

- New keys absent from `sdkconfig` are filled in on the next reconfigure or build.
- Changed keys that already exist stay stale; update those symbols with
  `idf.py menuconfig` (or a careful edit) to preserve other local settings.
- For a clean slate, back up first (`cp sdkconfig sdkconfig.local.bak`), delete
  `sdkconfig` and `sdkconfig.old` if present, then re-run `idf.py set-target <target>`.
  This discards local configuration changes; restore intentional overrides from
  the backup before building.

## Flash

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp32-node
idf.py -p <serial-port> flash monitor
```

## Main REPL Commands

After boot, the example starts a serial REPL on the board's primary console. Most targets use the same console as `idf.py monitor`.

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
openclaw nodes invoke --node <node-id> --command wifi.status --json
```

If pairing did not complete as expected, use [Troubleshooting](../../docs/troubleshooting.md).

## Use The Node

Get basic information:

```bash
openclaw nodes invoke --node <node-id> --command device.info --json
openclaw nodes invoke --node <node-id> --command device.status --json
openclaw nodes invoke --node <node-id> --command wifi.status --json
```

Configure and drive a GPIO pin with the stable documented path:

```bash
openclaw nodes invoke --node <node-id> --command gpio.mode --params '{"pin":<pin>,"mode":"input_output"}' --json
openclaw nodes invoke --node <node-id> --command gpio.write --params '{"pin":<pin>,"level":1}' --json
openclaw nodes invoke --node <node-id> --command gpio.read --params '{"pin":<pin>}' --json
```

Read ADC:

```bash
openclaw nodes invoke --node <node-id> --command adc.read --params '{"channel":0}' --json
```

## Other CLI Commands

Useful when you want to test more than the standard setup-code flow:

- `wifi set` rejects SSIDs or passphrases that do not fit the ESP-IDF station config exactly, instead of truncating them silently
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
- `gateway no-auth ...` performs one explicit no-auth connection attempt
- `gateway token ...` performs one explicit shared-token connection attempt
- `gateway password ...` performs one explicit password connection attempt
- `gateway connect` performs one reconnect attempt with the saved reconnect session
- `gateway disconnect` is valid only while the session is connected
- This example automatically retries the saved reconnect session after connection loss once Wi-Fi is back

</details>

## Troubleshooting And Reference

- [Troubleshooting](../../docs/troubleshooting.md)
- [Component README](../../components/esp-openclaw-node/README.md)

## Notes

- Choose a GPIO pin that is actually broken out, output-capable, and safe on your board before using `gpio.write`.
- The documented write path is `gpio.mode` with `"output"` or `"input_output"` followed by `gpio.write`.
- Open-drain GPIO modes are intentionally not part of the current command surface.
