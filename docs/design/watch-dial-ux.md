# Watch and Dial interaction plan

**Status: proposal for UX feedback.** The linked prototype uses simulated data;
it does not connect to devices or change firmware. The baseline is the tested
watch → Dial → spoken reply flow at `7c9384358172ecf3011c3b6db6a614c8c71a557e`.

Open [the interactive concept](watch-dial-ux.html) in a browser. Start with
**Voice → Dial**, then try **Connection lost** and **Request failed**. The state
selector lets you inspect each screen without waiting through the sequence.

## Direction

Make the watch a recognizable OpenClaw companion with one obvious next action.
Keep the mobile app's red lobster and turquoise eyes. Let its expression support
the state label; never make an expression or color the only explanation.

Keep the Dial's numeric control and physical interaction. Give it clearer
pending and completed outcomes, not a new personality or navigation system.

The first cut changes presentation and navigation. Pairing, authorization,
command schemas, audio scheduling, connection recovery, and action ownership
remain with their existing implementations.

## What stays true

| Surface | Existing contract | Design consequence |
| --- | --- | --- |
| Watch | 368×448 AMOLED, touch and BOOT button | Design at native size. Use a large labeled primary control and quiet black background. |
| Watch voice | One microphone; input is silenced during playback and its 300 ms tail | Say **Replying** and **Wait for the reply to finish**. Do not offer voice interruption or imply that the mic is physically powered off. |
| Watch rendering | Large lobster transforms pause during calls; eyes and mouth still animate | Preserve that budget. No full-screen animated gradients, waveform sweeps, image decoding, or per-frame allocation. |
| Watch state | Wi-Fi, Gateway, Talk availability and call lifecycle facts exist; current `SPEAKING` also means WebRTC connected | Use **In conversation** until trustworthy per-turn facts exist. Do not infer listening, processing, or actual playback from the current enum. |
| Dial | 240×240 round display, encoder, press, touch, buzzer; no microphone or speech speaker | Turning edits a value; pressing confirms. Speech comes from the watch. |
| Dial actions | Event acceptance and agent-reported completion are separate | **Request sent** is pending, not success. Display success only after an accepted `dial.complete` result. |
| Full node | Existing Canvas, commands, diagnostics and provisioning remain available | Simplify the default face without removing capabilities or exposing raw protocol details there. |

## Watch screens

The frame has three stable regions: a small connection/status line, the lobster
and current task, and a 52-pixel-high primary action. Text wraps inside the
screen; no marquee carries essential information.

| State | Main label and supporting text | Primary action | Mascot |
| --- | --- | --- | --- |
| Ready | **Ready when you are** · Tap below to start a conversation | **Talk to OpenClaw** | Gentle idle motion |
| Connecting | **Connecting…** · Opening your conversation | **End conversation** | Attentive, still body |
| Active, phase unknown | **In conversation** · Your conversation is connected | **End conversation** | Attentive eyes, still body |
| Listening | **I'm listening** · Ask your question | **End conversation** | Attentive eyes, still body |
| Working | **Working on it** · A short, grounded description when available | **End conversation** | Upward gaze, still body |
| Speaking | **Replying** · Wait for the reply to finish | **End conversation** | Mouth follows playback; body stays still |
| Result | **Dial is ready** · Volume · 42 · Range 0–100 | **Back to listening** while the call remains active | Brief smile |
| Offline | **Connection lost** · Reconnecting to OpenClaw | Disabled **Waiting for connection** | Quiet concerned expression |
| Request failed | **That didn't go through** · Couldn't reach the Dial | **Back to listening** while the call remains active | Concerned, no alarm loop |
| Details | Connection and voice status, with a visible Back action | **Back** | No animation needed |

The phase-specific **Listening**, **Working**, and **Replying** screens are
proposed presentations. Current code marks the call `SPEAKING` on WebRTC
connection; it does not reliably distinguish those phases. Use **In
conversation** in the first slice. Add finer labels only from producer-owned
capture, processing, and playback facts. A quiet microphone is not evidence of
processing; animation level is not evidence of semantic completion.

The result screen requires a typed successful command result, not optimistic
speech. Do not display battery percentage, time, or wake-word status without a
real source. The prototype demonstrates the intended presentations, not proof
that these UI events already exist.

When Talk is stopping, keep **Ending conversation…** disabled until the current
lifecycle completes. A failed startup returns to the real ready/unavailable
state; do not add a retry loop or make a disabled Talk state look ready.

Ending a conversation stops Talk. It does **not** promise to undo an action that
the agent has already sent. Show confirmed results without inventing rollback.

## Navigation

- Keep Start/End visible and in the same bottom position on the watch face.
- Add a labeled **Details** affordance. Preserve the existing long-press path
  to diagnostics; it becomes a shortcut rather than the only discovery path.
- Keep Canvas reachable through a labeled secondary action. While Canvas is
  visible, retain the conversation control in a reserved region so it does not
  cover content or steal a content tap. Decide the exact Canvas placement after
  testing real image/A2UI content; the first prototype focuses on conversation.
- A tap that wakes a dark display only wakes it. Require a subsequent deliberate
  press to start or stop Talk. Preserve the existing pressed-action validation
  across state changes.
- Preserve BOOT and existing gestures during the first cut. Do not assign swipe
  directions until we test conflicts with Canvas touch and diagnostics.
- Return to Ready after normal call timeout. Do not leave a stale Working,
  Speaking, or success screen after the call has ended.

## Dial behavior

Retain the current control hierarchy: label, large numeric value, unit, and an
arc. Keep **Turn to adjust · Press to confirm** discoverable. Touch dragging
edits the same value without sending an action. Holding returns to the face.

After a press, distinguish these outcomes:

1. **Sending…** while transport acceptance is pending.
2. **Request sent** after acceptance, while the agent's result is pending.
3. A short success or failure message from the accepted `dial.complete` result.
4. **Gateway offline** if the press could not be sent. Do not queue a replay.

Keep the confirmed value separate from the live editing value. If the user
turns from 42 to 55 while the result for 42 is pending, show **Editing 55** and
**42 pending**. A late reply must never look like confirmation of 55. Pending
and completed outcomes stay bound to the existing request/session/connection
identities; this is presentation of the current contract, not a new queue.

After transport acknowledgement, another press is allowed and supersedes the
previous outstanding result. Keep the existing correlation behavior; do not add
a completion lock or replay queue. The prototype permits a new press after its
simulated acknowledgement and discards a superseded result.

The prototype starts at the tested Volume 42 control. Its Dial button simulates
a confirmation and agent reply; it never controls actual volume. **Voice →
Dial** demonstrates configuration success, not proof of an external actuator.

## Delivery slices

| Slice | Change | Required evidence before landing |
| --- | --- | --- |
| 1 · Watch frame | Native-size layout, bottom Start/End, explicit availability and **In conversation** labels, unchanged lobster renderer | Inspected before/after captures; real touch wake/start/stop; Canvas/diagnostics reachability; no regression to long-call audio |
| 2 · Turn and result states | Map producer-owned processing/result facts into bounded labels; preserve timeout and failure behavior | Trace each displayed state to an actual event; no inferred success; failure and stale-event coverage |
| 3 · Dial outcomes | Pending/confirmed value distinction and clearer agent-reported outcomes | Turn/press/turn, disconnect, stale result, and late result after reconfiguration |
| 4 · Finish | Typography, wrapping, reduced motion, and idle/call animation tuning | Native-size review, long labels, camera recording of both devices, sustained voice run with zero dropped frames and watchdog events |

Do not widen slice 1 into new transport, persistent settings, protocol fields,
or permissions. If slice 2 needs a new public contract, discuss that separately
before implementation. The Dial facelift can wait; the watch is the priority.

## Feedback to settle

- Is the lobster large enough, with enough space for a clear state and action?
- Does the bottom conversation control feel easier to find than the current
  top pill?
- Is the result receipt useful, or should it collapse into one short line under
  the lobster while the conversation continues?

## Source anchors

- [Watch hardware and current behavior](../../examples/waveshare-esp32-s3-touch-amoled-1.8-room-node/README.md)
- [Dial controls and completion semantics](../../examples/m5stack-dial-node/README.md)
- [UI facts and view interface](../../components/esp-openclaw-room-node/room_ui_controller.h)
- [UI controller and gesture handling](../../components/esp-openclaw-room-node/room_ui_controller.c)
- [Connection and call fact producer](../../components/esp-openclaw-room-node/esp_openclaw_room_node.c)
- [Dial request correlation](../../examples/m5stack-dial-node/main/dial_action.c)
- [Lobster renderer and call-time animation budget](../../components/esp-openclaw-room-node/room_lobster.c)
- [Mobile artwork provenance and MIT notice](../../components/esp-openclaw-room-node/assets/NOTICE.md)

The prototype embeds derived mobile mascot geometry under the same MIT license.
Browser motion is illustrative; this is not a firmware performance benchmark.
