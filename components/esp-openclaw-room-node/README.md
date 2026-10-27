# esp-openclaw-room-node

The canonical OpenClaw room-node product component. It owns dual node/operator
sessions, reconnect and setup-code fallback, Talk lifecycle, ambient wake,
Canvas/A2UI, the responsive face, command registration, and startup policy.

Board examples provide three required narrow ports: display/input,
board-tested audio handles and AFE topology, and board services. An optional
storage port exposes an approved file root. Codec models, pins, panel
controllers, remote-Wi-Fi transport, and scheduler profiles remain outside
this component.

Non-animated displays use a static OpenClaw home with board identity, separate
Wi-Fi/Gateway/Talk facts, and bounded setup guidance or error details. Canvas and Diagnostics
cover the home without replacing those facts. The optional trailing
`display.idle_brightness` field accepts 0-100; omitted or zero preserves idle
display sleep, and explicit off requests remain unchanged. A nonzero value
keeps the backlight dimly lit and consumes more idle power. The home is a
status surface, not evidence that touch, camera, networking or Talk has been
qualified on a particular board.

The 1.8-inch Watch allocates LVGL objects, styles, and render data in PSRAM.
This preserves internal RAM for the radio and task stacks as menus grow; other
boards retain their configured LVGL allocator.

The local **Start Talk / Stop Talk** control uses the same admission and cleanup
as wake-word and remote Talk commands. It stays visible over the home, face,
and Canvas, and is hidden by diagnostics. Readiness comes from the operator and
media state; unavailable and stopping states disable the control. On boards
that sleep the display, the first touch on a dark Talk control only wakes the
display; tap the visible control again to start or stop Talk. If the
button's action changes during a press, release and press again; a held Start
press cannot become Stop when wake word or a remote command starts Talk.

The local USB command `agents refresh [offset]` reads a page of up to eight
agents through the existing operator connection. `agents` reports its state
and copied identities; `agents close` releases the page. This is read-only:
it does not select an agent or change an existing Talk session. The Gateway
currently returns the entire roster, so responses larger than 32 KiB are
rejected before parsing.

Directory requests wait during Talk startup and teardown. A 15-second
directory timeout does not disconnect the shared operator or interrupt Talk
and Canvas. Only one request can remain outstanding; a new refresh waits for
its predecessor to complete or be drained by disconnection. Reconnection or
a Gateway change invalidates the old page and late responses.

`talk.say {"text":"Set to 23 degrees."}` sends a short confirmation into the
current connected voice call. Text is limited to 1024 UTF-8 bytes. The response
`{"queued":true}` confirms local submission; a Gateway rejection appears on
the display without ending the call. A stopped, disconnected, or busy call
rejects the command. Add `talk.say` to the Gateway command allowlist and approve
the updated node surface. The Gateway must support `talk.client.say`.

The audio port's input gain override is optional. Boards that set
`configure_input_gain` also provide `input_gain_db`; otherwise shared media
initialization preserves the codec or board default.

The board-owned `playback_gain_db` is an optional 0–12 dB post-decode PCM16
boost; zero leaves playback samples unchanged. The shared renderer applies it
with fixed-point saturation immediately before diagnostics and I2S output.
Compressed media is never mutated. Boards with a physical speaker-reference
channel retain it for AEC. Boards without a reference channel, such as the
single-microphone `M` watch, use half-duplex capture: microphone PCM is replaced
with silence during non-silent speaker writes and for 300 ms of capture afterward.
Silent playback frames do not extend this tail. The
AFE and capture clock keep running, including ambient wake, but spoken
interruptions during playback are suppressed. Wait until the speaker finishes
before replying or confirming an action. This is turn-taking protection, not
acoustic echo cancellation; analog audibility and recognition still need a
physical check. Samples above the remaining digital
headroom saturate at PCM16 full scale; this is not a limiter or a guarantee
against analog distortion. Diagnostics shows the configured volume and boost.

Storage is explicitly optional. A board that supplies a canonical file root
gets the bounded file-transfer commands and storage metrics; boards such as the
Waveshare adapter advertise no file surface.

`dir.list` page tokens track directory entries visited, including entries skipped
because their metadata could not be read. Pass `nextPageToken` unchanged to
continue a listing. Pagination is not a snapshot; directory changes between
requests can still change the results.

`file.write` with `createParents: true` creates missing parent directories,
including the configured root if it disappeared after registration. Adding
`preflightOnly: true` validates a proposed write beneath an existing safe
ancestor without creating directories or files. A missing root or a disallowed
symlink still fails preflight.

`room_aec_src.c` carries the pinned `esp_capture` AEC/wake-event closure needed
by the current media stack: WakeNet only advances while its AFE fetch path is
drained. It lives here once, alongside the shared capture orchestration, until
the upstream capture component exposes the equivalent wake callback contract.
AFE/model open and close operations run on one 8 KiB internal-stack worker,
reserved when the audio source is created and retained across ambient/Talk
transitions. This avoids allocating a fresh contiguous stack after network and
playback activity has fragmented internal RAM. Include the worker in the board's
internal-RAM budget alongside both Gateway connections and playback; PSRAM
cannot serve these stack operations. If reservation fails, audio initialization
returns an error instead of postponing allocation until a call.
Node reconnects and operator startup/reconnects share the existing 6 KiB
internal-stack room supervisor with Talk teardown. Their timers are reserved
at startup and queue work without allocating another task. Teardown releases
an interrupted call before the next operator attempt; node reconnects remain
independent of active operator calls. The supervisor must stay in internal RAM
because client creation and saved-session reloads access NVS.

The closure retains Espressif's modified-MIT notice in
`LICENSE.ESPRESSIF-MODIFIED-MIT`. The OpenClaw home image retains its MIT notice
and source provenance in [assets/NOTICE.md](assets/NOTICE.md); the remaining
component source is Apache-2.0.

Long-press the status screen or an empty Canvas background to open the shared
Diagnostics overlay. The overlay keeps the current screen loaded beneath a
blocking, scrollable modal;
tap the large Close button (or long-press the modal) to return. Audio is shown
first: live MIC, post-AFE, and RX/SPK PCM meters include freshness, counters,
capture ownership, AFE/WakeNet mode, and renderer results.
“Renderer accepted” means the production renderer accepted PCM; it does not
prove that the codec, amplifier, or analog speaker produced audible sound.

The local speaker test queues a 1 second, 1 kHz PCM tone through the same
`av_render` → render FIFO → render tap → I2S renderer → codec/I2S TX path as
Talk playback. It is serialized against the complete Talk media lifetime and
runs from a worker task. Its result reports queued frames and renderer
acceptance only; the person at the device remains the audibility check.
Its input amplitude is reduced by the inverse board gain before entering that
same path, preserving tone level and headroom without a separate playback route.
Diagnostics presents this test as a lightweight six-object “tone buddy” card.
It updates on the existing 75 ms diagnostics refresh, reacts only while the
tone is running or during its eight-tick success bounce, and does not enable
the full-screen procedural face on boards that disable it.

Room endpoints request 400 ms of provider server-VAD post-speech silence for
each Talk session. The player uses the pinned 4 KiB raw and 6 KiB render queue
capacities: they start playback immediately and bound stale audio backlog under
stalls, but they do not measure or promise total speech-to-response time.
Talk WebRTC is audio-only. A compatible Gateway must own realtime control and
return the negotiated Gateway-control descriptor; older Gateways fail visibly
before the room creates a peer.

Session routing uses the Gateway's existing Talk configuration. On an explicit
multi-agent Gateway, select the intended agent with `talk.agentId`; the shared
[Talk adapter](../esp-openclaw-talk/README.md) prepares an owned main-session key
and retains it until the call closes. No firmware-specific agent setting is
needed, and a configuration change affects the next call rather than an active
one.

The USB REPL also exposes one local-only command with four exact forms:
`diagnostics open`, `diagnostics close`, `diagnostics tone`, and
`diagnostics status`. Open/close queue the modal operation onto the LVGL task;
tone queues the same asynchronous speaker test as the button. These are not
OpenClaw node commands and require no gateway allowlist entries. The open and
close commands report that work was queued; `diagnostics status` reports the
state after the LVGL task has executed it.

Audio diagnostics count an AFE feed as accepted only for a positive vendor
return; a full-ring drop returns zero and counts as an error. The console
and overlay also report the maximum interval between capture read calls and the
maximum time inside AFE fetch, in microseconds. These maxima reset when AFE starts
a new run. A long read interval with short fetches points to downstream scheduling
or queue backpressure; the values identify where to investigate, not its cause.

Talk admission freezes the authoritative operator handle, its authenticated
connection incarnation, the Gateway origin, and a monotonically increasing
call generation before the wake/command worker or media gate can run. Losing
that operator, deliberately rebinding its Gateway, or receiving an exactly
matched Gateway `session.closed` event cancels the admitted call. Reconnecting
never revives it. A node-role disconnect alone keeps the quiet reconnect path;
recoverable final errors do not terminate Talk.

All stop causes share a durable cancellation latch and a one-slot coalesced
worker wakeup. Media callbacks, setup failures, and per-call timeout IDs carry
the generation, so a reused WebRTC address cannot give an old action authority
over a replacement call. Teardown seals/drains the reusable Talk callback owner
before SDK close, restores ambient capture, releases the media gate, and updates
UI before making another call admissible. It runs only on the worker, including
canceled starts and failed setup. The operator Node is reused across reconnects
and remains alive for pending RPC cleanup.

The source-only [lifetime tests](tests/README.md) exercise these boundaries with
synthetic transports. Verify operator control loss and acoustic teardown on the
target board as well; host results do not establish audible continuation or
hardware behavior.

## Voice agent actions

Default firmware requests `operator.read` and `operator.talk`. Conversation and
the Gateway's read-only voice tools are available; spoken requests cannot run
mutating agent tools. Receiving authorized node commands, such as Canvas or
`talk.start`, is a separate capability.

To allow voice actions, prepare the credential **before** enabling the build
setting. Use a Gateway that supports `device.scopes.requestUpgrade` and
`device.scopes.waitUpgrade`:

1. Flash the default firmware and pair with a voice-node setup code. Wait for
   `operator Talk session ready` and stop any active call.
2. Run `voice-actions request` in the USB console. On the Gateway, run
   `openclaw devices list`, verify the exact request ID and device identity,
   and approve its `operator.read`, `operator.talk`, `operator.write`, and
   `operator.admin` scopes with `openclaw devices approve <requestId>`.
3. Run `voice-actions complete <requestId>` in the USB console. Wait for the
   new `operator Talk session ready` message: queued completion alone does
   not mean the replacement token has been saved.
4. Enable **OpenClaw room node → Request owner access for voice agent actions**
   (`CONFIG_OPENCLAW_ROOM_VOICE_ACTIONS=y`) in `idf.py menuconfig`, rebuild,
   and flash. Verify a real spoken action with an enabled agent tool. When the
   Gateway asks for confirmation of a high-impact action, answer aloud; owner
   access does not bypass that confirmation.

**This grants owner access**, including the configured agent's `nodes` tool
for spoken control of other devices and Gateway administration tools. It is
not limited to the paired watch or a single Dial. Enable it only for a trusted
voice device in a trusted environment; agent tool policy still applies.
The build option defaults to off and never approves its own request. Approval
rotates the old token, so keep power and connectivity until step 3 completes.
Interrupted preparation may require re-pairing with default firmware. Flashing
the enabled build before preparation fails authentication instead of creating
a pending approval. To return to read-only voice access, flash a build with
the setting disabled; the declared connection scopes then omit write and admin access.

An operator token that was explicitly narrowed after a previous owner approval
stays narrowed when the same scopes are approved again. If completion reports
insufficient access, a Gateway administrator must restore this device's token
scopes before re-pairing with a fresh voice-node setup code on default firmware:

```sh
openclaw devices rotate --device <device-id> --role operator \
  --scope operator.read --scope operator.talk --scope operator.write \
  --scope operator.admin
```

Keep the returned credential private. Use the voice-node setup code for USB
provisioning; do not paste the raw token into the console. Wait for operator
readiness before flashing the enabled build.
