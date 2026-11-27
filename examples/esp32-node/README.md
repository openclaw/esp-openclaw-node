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

Home opens the current conversation when one is selected, or the agent browser
when reader access is ready. Connection details and Help remain available
without Gateway access. The status strip distinguishes Wi-Fi from the Node's
Gateway connection; reader access is checked separately before browsing.

The compact profiles use a local workspace with Home, Agents, Conversations,
Devices, Activity, and Settings. Agent and conversation browsing use the shared
reader component; they require a separately approved operator session. A node-only
session still accepts node commands but cannot read conversations. Replies and
new conversations require messaging access and an explicit review/submit action.
In a conversation, choose Reply → Write message to compose up to 159 ASCII
characters. Choose a character group, select letters, and use Back for Space,
Delete and Preview. Review shows the recipient, conversation and complete text;
Send, Edit and Cancel remain separate choices. Home keeps an unsent draft while
the same preview is selected. If that selection changes, the saved draft stays
viewable but cannot be resumed or sent. Discard draft works even offline; choose
Write message again to start for the new selection. Sending clears the local
draft without automatically retrying it. A rejected local queue lets Edit restore
the reviewed text.
This device opens the local display and connection settings. Display brightness,
reading size, and list spacing are saved locally. Supported remote M5StickC and
T-Embed devices also offer Identify when connected, paired, and advertising
`display.show`. Open the device details, then actions, review the exact target
and replacement display code, and choose Show code. Messaging access is required.
The result distinguishes acceptance from confirmed rendering; an unknown outcome
is never retried automatically. Activity retains the latest Identify result and
returns to that captured target even after navigating elsewhere.

To add reader access to an already paired compact board, connect its USB console
and run `reader setup-code` without an argument. Enter a Gateway bootstrap setup
code at the hidden prompt within 30 seconds. The code is not echoed or added to
REPL history. It must target the same exact Gateway URI used by the node. Run
`reader status` to inspect the connection without exposing credentials.

The USB `reader heap` command reports free internal heap, its largest free block,
and the allocator's recorded minimum. These values help diagnose allocation
failures while the node and reader are connected. Both compact profiles let LVGL share the system heap instead of reserving a
separate pool for display allocations, and release idle TLS buffers.
It also reports the pending messaging request ID and revision so an operator
can match the exact Gateway approval before granting access.
Expired or cancelled input stays hidden until Enter ends the line. To cancel
an empty prompt, press Ctrl-C and then Enter.

For USB navigation, run `workspace status`, then use `workspace next <revision>`,
`workspace previous <revision>`, `workspace select <revision>`, or
`workspace home <revision>` with the returned `workspaceRevision`. Commands
queue one input at a time for the display owner. Status reports the request ID
and `queued`, `applied`, or `stale` outcome; `applied` means the input reached the
UI, not that a remote action completed. A view change before delivery rejects
the input as stale. Inspect the actual review/result screen before proceeding.
`consoleInputCount` and `physicalInputCount` remain separate.
USB status includes the selected row label for local navigation. Node-command
`display.status` omits that label, which can contain private conversation titles.

Approve only the expected existing device's `operator.read` role request on the
Gateway. If the first attempt was rejected while awaiting approval, run
`reader setup-code` again explicitly before the code expires. After connection,
discard the code; subsequent reconnects use the saved operator token. The node
identity and its independent connection remain in place. Settings → Messaging
access requests the separate read/write upgrade needed for reviewed Send and
Create actions. Reader setup itself does not request write, admin, or Talk access.

On the M5StickC, press A to select and B to move forward. Hold B to move backward;
hold A to return Home. On the T-Embed, turn the encoder to move and press to select;
hold it to return Home. Reading pages scroll with these same movement controls.
Back entries return to the previous view. A held key or a press whose view changes
before release cannot select a replacement action.

These profiles use a 3 MiB factory app partition. NVS remains at `0x9000`, PHY
data at `0xf000`, and the application at `0x10000`. Back up the current flash,
then flash the matching partition table and application when upgrading from the
earlier compact profile. Do not erase the whole chip; saved Wi-Fi and role tokens
remain in NVS. A separate partial display buffer leaves RAM for the reader.

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
characters, wrapped at word boundaries into four rows of 25 columns. Words
wider than a row are split; spaces at a wrap boundary are omitted. Body newlines
advance to the next row; input that exceeds four rows is rejected. The built-in
input accepts letters, digits, spaces, and `. , - _ : ! ? / + % ( ) =`.
Unsupported characters are rejected. Incoming messages open Activity, where they
can be scrolled. A reply review, conversation-creation review, or access request
keeps its focus; open Activity to read a message received during one of those flows.
Messages are kept in RAM and reset after reboot. Home and the menu remain available.

For short messages, add `"textSize":"large"` to `display.show`. This doubles
the body font size and fits two rows of 12 characters (24 characters total).
The heading and connection strip retain their normal size. Omit `textSize`
or use `"normal"` for the four-row layout. `display.status` reports the current
`textSize`, `textColumns`, `textRows`, and `textMaxLength`. Column and row values
describe the accepted input bounds. The workspace reflows messages with a
proportional font in a scrollable view; `layout: "scroll"` and `lineHeight` report
that presentation. `view`, `physicalInputCount`, and `lastInputMs` help verify
navigation without treating synthetic input as a physical press.

```sh
openclaw nodes invoke --node <node-id> --command display.show \
  --params '{"heading":"OpenClaw","text":"Ready","textSize":"large"}' --json
```

`display.show` queues the latest message. Its response and `display.status`
include `generation`, `renderedGeneration`, and `rendered`. A message is
confirmed drawn only when Activity is visible, `rendered` is true, and both generations match; an
accepted message can be replaced by a newer message before it is drawn.
`ready` becomes false if the display transfer fails. `device.info` reports
the selected board name and model identifier.

Both compact profiles synchronize their clock through SNTP after Wi-Fi obtains
an address and validate TLS certificate dates for `wss://` connections. Wait
for `clock synchronized for TLS certificate validation` in the serial console
before sending the first setup code. Synchronization runs in the background;
the console remains usable if the time server cannot be reached. Saved-session
reconnect can fail certificate validation until the clock is synchronized and
then retry normally. Existing local `sdkconfig` files must also enable
`CONFIG_MBEDTLS_HAVE_TIME_DATE=y`; profile defaults do not override saved values.
For a raw TLS passthrough relay, set `CONFIG_EXAMPLE_GATEWAY_TLS_SERVER_NAME`
in the untracked local configuration to the upstream certificate's DNS name.
This sets certificate name verification and SNI without disabling validation.
Leave it empty for a direct connection using the hostname in the Gateway URL.
See [LAN TLS passthrough](../../docs/lan-tls-relay.md) for the optional macOS relay.

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
