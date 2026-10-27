# Waveshare ESP32-S3 Touch AMOLED 1.8 watch node

This profile runs the shared OpenClaw room node on the **1.8-inch, 368×448**
Waveshare watch board with 16 MB flash and 8 MB PSRAM. It provides authenticated
node/operator sessions, reconnect, microphone capture, WakeNet, WebRTC Talk,
the mobile app's animated OpenClaw lobster, image/A2UI Canvas, and the shared
diagnostics overlay.

Use this profile only for the 1.8-inch board. The 2.06-inch board has different
display, touch, and audio hardware. A successful flash does not identify a
compatible board; the wrong image can enter a boot loop during touch setup.

## Build and pair

Use ESP-IDF 5.5.5 and initialize the repository's submodules before building.
Back up the device before replacing factory firmware or its partition table.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <watch-serial-port> flash monitor
```

Generate a voice-node setup code on a reachable Gateway:

```sh
openclaw qr --url ws://<gateway-host>:<gateway-port> --voice-node --setup-code-only
```

Use the USB console to provision the watch:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <setup-code>
openclaw> status
```

Follow [Getting Started](../../docs/getting-started.md) for Gateway reachability
and command allowlists. This board uses the same room-node commands described
in the [shared command reference](../waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md#canvas-commands).
Canvas coordinates use this board's **368×448** dimensions.

For a live voice check, follow the shared
[Talk verification steps](../waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md#verify-talk).
Voice-agent write actions require the shared
[opt-in and Gateway approval steps](../../components/esp-openclaw-room-node/README.md#voice-agent-actions);
the default firmware uses read-only voice access.

Tap the screen or press BOOT to wake the display, then use **Start Talk** at the
bottom of the screen. During connection and conversation it becomes **Stop Talk**.
It is disabled while Talk is unavailable or stopping. The control also appears
over Canvas; the existing screen tap/hold gestures still toggle Canvas and open
diagnostics. Diagnostics hides the Talk control until closed.

The watch Home keeps Wi-Fi and Gateway status above the lobster, with a bounded
headline and a separate bottom action. A live connection says **Conversation
live**; it does not infer speech playback from the connection state. Missing
clock and battery telemetry are omitted. The display still sleeps when idle;
waking it does not start a conversation. Idle, setup and error views keep static
mascot artwork without running the animation timer; wake-up hints still animate.
When a retained reader is available, Home shows its agent and **Continue
conversations** above the separate Talk button. Continue restores the reader
without changing the next Talk agent; a reader changed during the tap is not
opened. Without a reader, a selected next-Talk agent appears in the compact
headline. Setup, errors, and active calls take priority over the Continue card.
Agents shows local fox and lobster artwork for configured 🦊 and 🦞 identities,
with an ASCII symbol or name initials for other identities. The same avatar
appears in the list and detail view; it does not change agent routing.

The **Menu** button opens Agents, Conversations, Canvas, Connection, and Settings. Connection
shows live Wi-Fi, Gateway, and Talk states. Canvas shows the retained card or
an empty-state explanation. Home returns to the latest conversation state;
Start/Stop Talk remains available on menu pages. Device status opens the
existing diagnostics overlay from Settings. The first touch on a dark screen only wakes it.

Conversations returns to the last reader and its scroll position while the
same content is still current. Leaving for Menu or Home keeps that reader in
memory and releases the hidden display buffers; returning does not fetch it
again or change the next Talk agent. With no retained reader, it opens Agents.
Closing the reader, refreshing Agents, reconnecting, or restarting clears it.

Settings → Display adjusts awake brightness in five-point steps from 10% to
100%. The preference lasts until restart; an unsuccessful change keeps the
last accepted value and shows a retry message. It applies to Home, Talk,
Canvas and diagnostics, while idle sleep still turns the display off. Camera
use keeps the screen at least 40% bright and labels that minimum when it
exceeds the selected value. Brightness and navigation work offline and never
change the selected agent or end a call.

The USB console can open the same pages with `menu`, `menu connection`,
`menu conversations`, `menu settings`, `menu display`, `menu canvas`, and `menu home`, without starting or stopping a conversation.
Use `brightness <10..100>` to set the same runtime preference over USB.
Additional settings and press-to-record messaging are tracked in the
[interaction plan](../../docs/design/watch-dial-interaction-plan.md).

## Board behavior

The watch mascot uses the mobile app's red lobster artwork and turquoise eyes.
At 20 frames per second, its claws and antennae move, it blinks and looks around,
and its mouth follows speech playback. During voice calls, the body, claws,
and antennae rest so audio
processing has priority; blinking, gaze, and the speech-reactive mouth continue.
Unchanged poses do not schedule display redraws. The existing `face.set` moods
and `face.gesture` commands still apply. Animation pauses when the mascot is
hidden; Talk controls, Canvas, diagnostics, and display sleep keep their
existing behavior.

The pinned Waveshare BSP 2.0.3 detects CST816S or FT5x06 touch and applies the
CO5300 panel offset required by the CST816S revision. The adapter registers
this QSPI panel through the LVGL SPI path; the BSP's high-level RGB display
helper is incompatible with this controller. Two 8-row internal DMA
buffers keep display transfers bounded and leave room for the AFE worker and
both Gateway connections, while PSRAM holds the larger UI/audio allocations.
Touch and BOOT-button input use the shared room interface; long
press opens Diagnostics.

The adapter also applies the [board's documented expander reset sequence](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/examples/esp-idf/90_axp2101_pmu/components/board_variant/board_variant.c)
to the panel-power, panel-reset, and touch-reset pins before initialization.

The watch has one ES8311 codec serving both microphone and speaker. One codec
instance and one paired I²S bus serve separate capture and playback handles,
so stopping playback cannot power down an active microphone. Capture declares
one microphone (`M`), with no hardware speaker-reference channel. Wake word and
Talk are available, but this profile does **not** claim acoustic echo
cancellation. The shared audio path silences microphone PCM during playback
and for a 300 ms capture tail to keep replies from becoming new user turns.
Voice interruption is unavailable while the watch speaks; wait for it to
finish before answering. Start with the default 70% speaker volume and no
digital boost.

The [shared room component](../../components/esp-openclaw-room-node/README.md)
owns face animation, Talk, Canvas, session behavior, and diagnostics. This
example supplies board wiring and audio topology. It does not advertise a
camera, SD file surface, or the Dial's rotary controls.

CI builds and packages the watch image. Physical qualification separately
checks touch, display alignment, microphone/speaker behavior, pairing,
reconnect, wake word, and a live Talk session.
