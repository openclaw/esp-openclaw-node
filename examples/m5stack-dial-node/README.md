# M5Stack Dial node

Run the M5Dial as an OpenClaw node with its 240×240 touch display, rotary encoder,
press button, and buzzer. It shares authenticated pairing, NVS-backed Wi-Fi and
Gateway sessions, reconnect, device status, and USB provisioning with the other
examples. The Dial has no microphone or PSRAM; use the
[Waveshare watch](../waveshare-esp32-s3-touch-amoled-1.8-room-node/README.md)
for voice, wake word, and the image/A2UI Canvas.

The display uses one 9.6 KiB DMA buffer. The face uses LVGL primitives, with no
image assets or audio stack. The BSP owns touch, encoder, and button drivers;
the application uses its existing LVGL input device.

## Build and pair

Use ESP-IDF 5.5.5 and the checked-in defaults:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <dial-serial-port> flash monitor
```

Back up the existing firmware before changing the device's partition layout.
Generate a setup code on a reachable Gateway, then provision over USB:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

See [Getting Started](../../docs/getting-started.md) for the Gateway's LAN and
command-allowlist requirements. Preserve existing allowlist entries when adding
the commands below. Rebooting reuses the saved session; no rebuild is needed
when Wi-Fi or the Gateway changes.

## Controls

The screen is upright with the USB connector on the right. The home screen
shows a blinking face and connection status. Turn the encoder
or tap to open the numeric control. Turn or drag its arc to change the value;
press the encoder or tap the center to confirm. Hold the encoder or the center
of the screen to return to the face. Dragging the arc never confirms an action
or triggers the home gesture. Turning continues to adjust after confirmation.

`dial.configure` defines the control. Its values must be integers between
−10000 and 10000, with a positive step and 1–1000 equal intervals. Both the
maximum and initial value must align with the minimum/step grid. Invalid updates
leave the current control intact.

```json
{"label":"Room brightness","min":0,"max":100,"step":5,"value":50,"unit":"%"}
```

An optional `action` name binds confirmation to the agent session that invoked
`dial.configure`. Invoke it from that session so the Gateway supplies its
session key. The device sends one `agent.request` through `node.event` when the
user presses; rotary ticks never create agent turns. The message reports
`DIAL_INPUT`, the action name, value, and unit. The agent decides what to do with
it. Commands do not execute arbitrary local shell or GPIO operations.

Only one confirmation can be pending. A replacement configuration is rejected
until that result arrives. Offline presses show `Gateway offline` and are not
replayed after reconnection. `Action sent` means the Gateway accepted the event,
not that an external device completed an action.

## Commands

| Command | Parameters | Result |
| --- | --- | --- |
| `device.info`, `device.status`, `wifi.status` | `{}` | Shared device/network status |
| `display.show` | `{"heading":"Hello","text":"A message"}` | Display state; limits: 64/512 UTF-8 bytes |
| `display.clear` | `{}` | Return to the face |
| `display.status`, `dial.status`, `input.status` | `{}` | View, value/range, input counts, brightness, connection, action state |
| `display.brightness` | `{"percent":65}` | Set backlight, integer 0–100 |
| `dial.configure` | Numeric control above, optional `unit` (16 bytes) and `action` (64 bytes) | New control state |
| `face.set` | `{"mood":"happy"}` | Show face; neutral/happy/excited/thinking/sleepy/sad |
| `buzzer.play` | `{"frequencyHz":1000,"durationMs":200}` | Start a tone; 100–4000 Hz, 20–2000 ms |

The buzzer stops asynchronously and rejects overlapping tones. It uses LEDC
timer/channel 0; the display retains the BSP's separate backlight timer/channel.
RFID, RTC, and external GPIO devices are not advertised by this example.

The USB console can exercise the same handlers before pairing. For example:

```text
local display.status {}
local face.set "{\"mood\":\"happy\"}"
local buzzer.play "{\"frequencyHz\":1000,\"durationMs\":200}"
```

`local` accepts only the Dial commands listed above. It has no agent-session
identity, so it cannot bind agent actions. JSON quoting is required by the
console parser; the complete command must fit its 512-byte input limit.

## Validation

CI builds this example and publishes its firmware through the normal
[CI artifact flow](../README.md#ci-firmware-downloads). Host tests cover range
validation, endpoint arithmetic, and preservation of the old value after invalid
configuration. Real LVGL input tests cover turn/confirm/turn, stationary holds,
and short/sustained touch drags. Run them after resolving the BSP dependencies:

```sh
python3 tests/run_input_host_tests.py --lvgl-dir managed_components/lvgl__lvgl
```

Physical qualification additionally checks boot/pair/reconnect,
touch and rotary operation, screen output, and audible buzzer playback.
