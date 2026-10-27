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

### Read access

The Dial also starts a separate operator connection after the node connects.
It requests only `operator.read` and reuses the operator session handed off
during pairing. It does not grant itself access or change the node connection's
permissions. This is the connection foundation for agent and conversation
browsing; those pickers are not yet part of the firmware UI.

Run `reader` in the USB console to inspect it:

```json
{"state":"ready","readConnected":true}
```

The states are `offline`, `connecting`, `ready`, `access-needed`, `error`, and
`unavailable`. `access-needed` means the saved operator session is missing,
rejected, or belongs to a different Gateway. Pair again using the Gateway's
supported operator handoff profile; a normal node-only pairing may not grant
read access. Transport losses retry automatically; missing credentials and
authentication rejections wait for a new node connection. An allocation error
can temporarily report `error` while retrying; `unavailable` means the reader
worker could not start. Numeric controls and node commands remain usable in
every reader state.

Read readiness requires both clients to be connected to the same Gateway in
the current node connection. Node reconnects immediately invalidate the old
reader state, and re-pairing reloads the role-specific saved session. The
console status does not expose Gateway addresses or credentials.

## Controls

The screen is upright with the USB connector on the right. The home screen
shows a compact connection strip, a blinking face, and one action. Wi-Fi and
Gateway status are separate; no battery level is invented. Home shows whether
an agent action is assigned and retains the current action's feedback in its
headline. Open dial works locally; Open control returns to an assigned control.
Turn the encoder
or tap to open the numeric control. Turn or drag its arc to change the value;
press the encoder or tap the center to confirm. Hold the encoder or the center
of the screen to return to the face. Dragging the arc never confirms an action
or triggers the home gesture. Turning continues to adjust after confirmation.
If a command changes the view or reconfigures the control during a held press,
release and press again to interact with the new view or confirm the new value.

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
`DIAL_INPUT`, a `requestId`, the action name, value, and unit. The agent decides what to do with
it. Commands do not execute arbitrary local shell or GPIO operations.

Only one confirmation can be pending. A replacement configuration is rejected
until that result arrives. Offline presses show `Gateway offline` and are not
replayed after reconnection. `Action sent` means the Gateway accepted the event,
not that an external device completed an action.

After carrying out an action, the same agent session can invoke `dial.complete`
with the event's `requestId`, a boolean `success`, and a short `text` message:

```json
{"requestId":"7-3","success":true,"text":"Brightness set to 65%"}
```

This displays the outcome without replacing the control. It accepts each result
once, and rejects results from another session, an older press, a replaced
control, or a previous connection. A result can arrive before the event's
transport acknowledgment without being overwritten by that acknowledgment.
`dial.status` includes `requestId`, the captured `confirmedValue`, and
`actionCompleted` while that press is current. A completed result also includes
`actionSucceeded` and `actionMessage`. Turning afterward changes `value`, not
the captured confirmation. This is an agent-reported outcome, not independent
verification of the external action. Speech on the watch is a separate step.

Actions stay bound to the Gateway URI that configured them. Reconnecting to
that URI preserves the control, but cancels any pending press. Connecting to a
different URI clears the action binding, retains the numeric value, and shows
`Configure action again`. Configure the control from an agent session on that
Gateway to enable presses again. Late replies from an older press cannot
change the result of a newer one.

## Commands

| Command | Parameters | Result |
| --- | --- | --- |
| `device.info`, `device.status`, `wifi.status` | `{}` | Shared device/network status |
| `display.show` | `{"heading":"Hello","text":"A message"}` | Display state including `heading` and `text`; limits: 64/512 UTF-8 bytes |
| `display.clear` | `{}` | Return to the face |
| `display.status`, `dial.status`, `input.status` | `{}` | View, value/range, input counts, brightness, connection, action state |
| `display.brightness` | `{"percent":65}` | Set backlight, integer 0–100 |
| `dial.configure` | Numeric control above, optional `unit` (16 bytes) and `action` (64 bytes) | New control state |
| `dial.complete` | `requestId`, boolean `success`, `text` (128 bytes) | Outcome for the latest press, from its configuring agent session |
| `face.set` | `{"mood":"happy"}` | Show face; neutral/happy/excited/thinking/sleepy/sad |
| `buzzer.play` | `{"frequencyHz":1000,"durationMs":200}` | Start a tone; 100–4000 Hz, 20–2000 ms |

Display-state responses include `heading` and `text` while `view` is `message`,
so an agent can read back the displayed content. Other views omit those fields;
`display.clear` returns to the face without reporting the hidden message.

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
