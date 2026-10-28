# M5Stack Dial node

Run the M5Dial as an OpenClaw node with its 240×240 touch display, rotary encoder,
press button, and buzzer. It shares authenticated pairing, NVS-backed Wi-Fi and
Gateway sessions, reconnect, device status, and USB provisioning with the other
examples. The Dial has no microphone or PSRAM; use the
[Waveshare watch](../waveshare-esp32-s3-touch-amoled-1.8-room-node/README.md)
for voice, wake word, and the image/A2UI Canvas.

The display uses one 9.6 KiB DMA buffer. The face uses LVGL primitives;
agent avatars use small local image assets. There is no audio stack. The BSP owns
touch, encoder, and button drivers; the application uses its existing LVGL input
device.

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
By default it requests only `operator.read` and reuses the operator session handed off
during pairing. It does not grant itself access or change the node connection's
permissions. The on-device agent browser uses this connection. Conversation selection
is a separate feature.

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

### Agent directory

Use the read connection to fetch the Gateway's agent roster:

```text
agents refresh
agents
agents refresh 8
agents
```

`refresh` starts an asynchronous `agents.list` read. `agents` inspects its result
without restarting it. Each page contains at most eight IDs, names and identity
emoji, plus the total count and local page offset. Identity names take precedence
over configured names. Avatar URLs, workspace paths and other agent metadata
are not returned by the device console. This is the data path for the upcoming
on-screen picker; it does not select an agent or change any configured control.

Directory states are `idle`, `loading`, `ready`, `empty`, `offline`, `connecting`,
`access-needed`, `error`, and `too-large`. An empty successful response is not a
failure. Reconnects free old pages before the next handshake and refresh a previously requested
directory. A newer refresh supersedes an older result; only one request can be
outstanding. A request with no response after 15 seconds closes only the reader
connection and reports an error; run `agents refresh` to retry. During a messaging
approval, the read still reports its error at 15 seconds, but transport cleanup
waits for approval to finish or expire. Node commands
and numeric controls remain independent.

The Gateway currently sends the whole roster, so pages do not avoid receiving
that response. The parser accepts at most 32 KiB; the transport's complete-frame
limit may be reached earlier. Malformed, too deeply nested or memory-constrained
responses produce an error instead of a partial roster. Listing an agent does
not guarantee access to a later conversation or device action.

## Controls

The screen is upright with the USB connector on the right. The home screen
shows a compact connection strip, a blinking face, and one primary action. Wi-Fi and
Gateway status are separate; no battery level is invented. The same strip stays
visible above navigation pages. Signal refreshes once per second while browsing
with the display awake, without interrupting a held selection. Compact keeps
three rows and Comfortable keeps two; edge rows narrow to fit the round screen.
Home shows whether an agent action is assigned and retains a short status for the
latest action.
With a retained reader selection, Home shows the agent name: press or tap
**Continue** to return to its preview, or **Conversations** to return
to its list. With no selection, **Choose agent** opens Agents. Offline,
**Connection** opens the network summary; Back returns directly Home.
The Home header uses the same configured fox or lobster avatar as the agent
browser. Other identities use one initial in this compact header. Clearing
the reader selection restores the generic Agents icon.

Tap the agent name (or **Agents**) to open the agent browser, or the menu icon
to open all sections. The face and background do not activate anything. The
primary action has its own touch target; pressing the encoder activates it too.

Turning on Home always opens the menu, which contains Agents, Conversations,
Devices, Activity, and Settings. Turn to move the highlight and press to open; touch
rows directly. Hold the encoder or a stationary touch to return Home. A reader
refresh, replacement, or disconnect invalidates an in-progress Home press;
continuing never submits a numeric control or changes its owning agent.

Settings → Display → Brightness adjusts this Dial's screen brightness. Turn the
encoder or tap − / + for five-point steps between 10% and 100%; press to return
to Display.
The page shows the last successfully applied value and an error if a change
fails. It works offline, never submits the configured numeric control, and
keeps focus during Gateway reconnects. Brightness lasts until restart; the
existing `display.brightness` command updates the same value and retains its
0–100% range. Hold still to return Home from any settings page.

Settings → Display → Reading text offers Standard and Larger text. It reflows
conversation previews, quick-reply reviews, and other scrollable reading views.
The choice is local to this Dial and lasts until restart. Menus and status icons
keep their existing size; this does not change the assigned control.

**Display → List spacing** switches between Compact (three rows) and Comfortable
(two taller rows) in menus, agent and conversation pickers, device lists, and
quick-reply choices. Turn to reveal the remaining rows and press to choose; Back
returns to the same selected item. Both layouts retain at least 44-pixel row
height within the circular display. Compact is the default and the selection
lasts until restart. It does not change text size or the assigned control.
Browse lists use 16-pixel text, increasing the selected label to 20 pixels when
it fits. Long names stay on one line with an ellipsis; open
the item to read its full identity.

Settings → Connection is a read-only, scrollable network summary. Turn the
encoder or swipe to read the saved network, signal and channel (when available),
current IP address, and independent Gateway state. Offline details include the
last disconnect reason code and USB recovery guidance. An unavailable AP
observation is shown as unavailable; disconnected pages hide old IP/signal data.
The page refreshes once per second without resetting the reading position.
Press the encoder or back arrow to return to Settings (or This Dial when opened
there), or Home when opened from Home's offline action. Opening the page starts
at the top. The USB command
`local display.menu "{\"page\":\"connection\"}"` opens the same page.
Display-state responses include `wifiConnected` alongside the existing Gateway
`connected` flag. Phone pairing remains planned work; this page does not offer a
nonfunctional pairing or retry button.

Settings → Messaging shows this Dial's messaging permission. It starts off with
read-only access. **Request** asks the Gateway for approval; the page
then shows whether the request is being sent, waiting for approval, or reconnecting
to check the grant. **Access ready** appears only after the reader confirms its
authenticated access. This page manages permission; it does not send messages.

Back and a long press preserve a pending request. **Cancel** stops the local
request and returns the reader to read-only access. Once ready, **Turn off**
restores read-only access on this Dial. Neither action revokes a grant already
approved on the Gateway. Rejected, expired, and failed requests can be
requested again. If the Dial is offline, reconnect before retrying. The node's
configured numeric control is unchanged. The USB command
`local display.menu "{\"page\":\"messaging\"}"` opens the same page.

Cancel stops the local wait; dismiss the pending approval on the Gateway too.
Approving it afterward can rotate the saved operator token and require pairing
again. A Gateway connection change or restart resets messaging to read-only.

Agents shows a bounded page of the Gateway roster. Turn to browse names, then
open an identity to see its canonical ID. The back arrow returns to the list;
Next agents, Previous agents, Refresh, and Back appear after the names.
Long names and IDs scroll in the detail view.
Loading, offline, missing read access, empty pages, and errors have distinct
screens. A refresh or disconnect cancels any held selection before replacing
the rows. Configured 🦊 and 🦞 identities use local artwork in the list and detail view.
Other identities use an ASCII symbol or name initials.
This is read-only browsing: opening an agent does not select a conversation or
change which agent owns a configured numeric control.

Conversations returns to the last browsed agent's list, highlighting the last
opened conversation. Home, Controls, and Settings retain that bounded reader
snapshot; returning does not submit an action or make another list request.
With no selection, Conversations opens Agents. Refreshing Agents, a reconnect,
restart, or `conversations close` clears the selection and requires choosing
an agent again. The selection lasts only for the current reader connection.

Devices opens a bounded, read-only inventory from the current Gateway. **This
Dial** stays available offline and opens Control, Display, and Connection.
Display and Connection return to This Dial when opened there, or to Settings
when opened from Settings. Remote rows show their reported connection state;
open one to read its full name, model, connection, and pairing state. Missing
metadata appears as Unknown. Turn to scroll details; press or tap Back to return
to the same list position. Refresh requests a new inventory; turning through
pages does not send another request. A replacement or disconnected inventory
cancels a held selection before changing the rows. Returning from This Dial
refreshes the inventory. This list does not pair devices or send remote commands.

Devices → This Dial → Control opens the existing numeric dial. Turn or drag its arc to change the
value; press the encoder or tap the center to confirm. Dragging never confirms
an action or triggers Home. Turning continues to adjust after confirmation.
Hold the encoder or a stationary touch to return Home from the control.
Tap the back arrow to return to This Dial without confirming the value.
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
until the transport outcome arrives. Offline presses show `Gateway offline`
and are not replayed after reconnection. **Waiting for result** means the Gateway
accepted the event, not that an external device completed an action.

Activity opens the latest submitted control request directly. It retains the
submitted label, value, and unit even if the dial changes afterward. Turn or
swipe to read the result; press Back to return to the Activity menu row, or hold
to return Home. Reading Activity never submits or retries a control.

The page distinguishes Sending, Waiting for result, Completed, Failed, Not sent,
and Outcome unknown. An uncertain transport failure can still receive a matching
completion while its connection and binding remain current. Disconnecting or
replacing that binding makes an unfinished outcome unknown: check the target
before retrying. Confirmed outcomes survive those changes. Only the latest
request is retained in memory, until restart; this is not persistent history.

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

Home's compact status strip shows measured Wi-Fi signal separately from the
OpenClaw link. A crossed icon is disconnected; neutral Wi-Fi arcs with a lit
dot mean connected with unavailable signal data. The battery outline and dash
mean unknown, not empty. Signal is refreshed once per second while Home is
visible. Settings → Connection keeps the text description of each connection.

| Command | Parameters | Result |
| --- | --- | --- |
| `device.info`, `device.status`, `wifi.status` | `{}` | Shared device/network status |
| `display.show` | `{"heading":"Hello","text":"A message"}` | Display state including `heading` and `text`; limits: 64/512 UTF-8 bytes |
| `display.menu` | `{}` or `{"page":"devices"}` | Open `menu`, `agents`, `conversations`, `preview`, `devices`, `activity`, `settings`, `display`, or `connection` |
| `display.clear` | `{}` | Return to the face |
| `display.status`, `dial.status`, `input.status` | `{}` | View, value/range, input counts, brightness, connection, action state |
| `display.brightness` | `{"percent":65}` | Set backlight, integer 0–100 |
| `dial.configure` | Numeric control above, optional `unit` (16 bytes) and `action` (64 bytes) | New control state |
| `dial.complete` | `requestId`, boolean `success`, `text` (128 bytes) | Outcome for the latest press, from its configuring agent session |
| `face.set` | `{"mood":"happy"}` | Show face; neutral/happy/excited/thinking/sleepy/sad |
| `buzzer.play` | `{"frequencyHz":1000,"durationMs":200}` | Start a tone; 100–4000 Hz, 20–2000 ms |

Display-state responses include `heading` and `text` while `view` is `message`,
so an agent can read back the displayed content. Menu responses include
`view: "menu"` and `page: "menu"`, `"agents"`, `"agent"`, `"conversations"`,
`"preview"`, `"activity"`, `"settings"`, `"display"`, or `"connection"`. Other views omit those fields;
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

### Device inventory integration

The reader exposes `dial_reader_devices_refresh`, `dial_reader_devices_copy`,
and `dial_reader_devices_close` for device views. They use the shared bounded
`node.list` owner through the existing `operator.read` connection. No extra
scope, client, device command, or control-session assignment is requested.

Inventory copies clear immediately when the node or reader connection is no
longer current. Closing a view invalidates late replies. A 15-second inventory
timeout leaves the reader connection usable and requires an explicit refresh;
it does not disconnect a configured control. Agent and conversation requests
can still complete while an old inventory callback drains.

### Read-only conversations over USB

Refresh `agents`, then use `conversations open <agent-id>` to request that
agent's first page. `conversations` prints the last result; `conversations
refresh <offset>` requests another page using the returned `nextOffset`.
`conversations preview <index>` requests up to four messages for that exact
returned row. `conversations close` clears the browser state.

These USB commands use the existing independent `operator.read` client. They
neither change the session assigned by `dial.configure` nor send a message. Opening requires
a current agent-directory row; reconnects, a new roster, and leaving the
browser invalidate old replies. Missing previews remain distinct from empty
transcripts. Pages and previews share a single bounded request slot and its
15-second response deadline.
Open **Agents**, choose an agent, then press **Conversations**. Turn the dial to
choose a row and press to read its preview. In the preview, turning scrolls;
tapping Back returns to the selected row. Pressing the encoder or tapping
**Reply** opens quick replies for a loaded or empty conversation. Back from the list returns
to that agent. Holding still returns Home. A drag scrolls without selecting.
Loading, empty, missing, offline and failed reads replace the previous content.

For a remotely selected USB browser context, `display.menu` also accepts
`{"page":"conversations"}` and `{"page":"preview"}`. These show the current
validated selection; they do not choose a new agent or conversation. A preview
is limited to the Gateway's returned messages, not a full chat history. The USB
commands remain read-only.

Conversation previews, Activity details, and remote-device details stop at both
ends when scrolling with the encoder, including rapid multi-detent turns.
Reaching an edge does not wrap, change pages, or submit the configured control.

### Reviewed quick replies

From a conversation preview, choose **Reply**, then **Continue**, **Summarize**,
or **Explain more**. Missing messaging access opens a route to **Settings →
Messaging**; Back returns to the reply flow after approval. Choosing a preset
only prepares a draft. Review the complete agent identity, conversation title,
and exact message by turning or scrolling, then open **Actions → Send** to
submit it explicitly. **Presets** chooses different wording; Back from the review before Send
discards the unsent draft.

Sending waits for the Gateway; Accepted waits for the agent. Sent confirms the
reply was admitted, not that agent work finished. **Read reply** refreshes the
same conversation when it remains accessible. An unknown outcome asks you to
check that conversation before trying again. Back or Home after Send hides the
status without aborting the submitted turn; Reply reopens an active status,
even if the visible preview has changed. A new reply is available after the
previous turn reaches an outcome.

Quick replies use the separately approved messaging connection. They do not
change the numeric control's configured action or owner, and require no
microphone. A changed permission, target, or reply state cancels a held press.

### New conversations

Open an agent's **Conversations** list and choose **New chat**, including when
the list is empty. Review the full agent name and ID, then press **Create** to
create an empty conversation. This sends no initial message and does not reset
the agent's main conversation. If messaging access is missing, **Messaging**
opens its approval settings; Back returns to this review. Creation remains
unavailable until the agent is verified again after reconnecting.

**Creating** is pending, and **Created → Open** opens the returned conversation
preview. Back before Create discards the review. Back or Home after Create only
hides the status; a created empty conversation remains if you leave. An unknown
outcome offers **Chats** to refresh that agent's list before an explicit new
attempt. A failed request offers **Retry**, which prepares a new review and
still requires Create. Numeric controls and existing quick replies keep their
current owners.
