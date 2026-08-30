# esp-openclaw-room-node

The canonical OpenClaw room-node product component. It owns dual node/operator
sessions, reconnect and setup-code fallback, Talk lifecycle, ambient wake,
Canvas/A2UI, the responsive face, command registration, and startup policy.

Board examples provide three required narrow ports: display/input,
board-tested audio handles and AFE topology, and board services. An optional
storage port exposes an approved file root. Codec models, pins, panel
controllers, remote-Wi-Fi transport, and scheduler profiles remain outside
this component.

The audio port's input gain override is optional. Boards that set
`configure_input_gain` also provide `input_gain_db`; otherwise shared media
initialization preserves the codec or board default.

The board-owned `playback_gain_db` is an optional 0–12 dB post-decode PCM16
boost; zero leaves playback samples unchanged. The shared renderer applies it
with fixed-point saturation immediately before diagnostics and I2S output.
Compressed media is never mutated, and the boards retain their physical
speaker-reference capture path for AEC. Samples above the remaining digital
headroom saturate at PCM16 full scale; this is not a limiter or a guarantee
against analog distortion. Diagnostics shows the configured volume and boost.

Storage is explicitly optional. A board that supplies a canonical file root
gets the bounded file-transfer commands and storage metrics; boards such as the
Waveshare adapter advertise no file surface.

`room_aec_src.c` carries the pinned `esp_capture` AEC/wake-event closure needed
by the current media stack: WakeNet only advances while its AFE fetch path is
drained. It lives here once, alongside the shared capture orchestration, until
the upstream capture component exposes the equivalent wake callback contract.
The closure retains Espressif's modified-MIT notice in
`LICENSE.ESPRESSIF-MODIFIED-MIT`; the rest of this component is Apache-2.0.

Long-press the status screen or an empty Canvas background to open the shared
Diagnostics overlay. Non-animated status screens show `Hold for diagnostics`
near the bottom; the hint is hidden on Canvas and while the modal is open. The
overlay keeps the current screen loaded beneath a blocking, scrollable modal;
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

The USB REPL also exposes one local-only command with four exact forms:
`diagnostics open`, `diagnostics close`, `diagnostics tone`, and
`diagnostics status`. Open/close queue the modal operation onto the LVGL task;
tone queues the same asynchronous speaker test as the button. These are not
OpenClaw node commands and require no gateway allowlist entries. The open and
close commands report that work was queued; `diagnostics status` reports the
state after the LVGL task has executed it.
