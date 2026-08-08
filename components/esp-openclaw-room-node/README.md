# esp-openclaw-room-node

The canonical OpenClaw room-node product component. It owns dual node/operator
sessions, reconnect and setup-code fallback, Talk lifecycle, ambient wake,
Canvas/A2UI, the responsive face, command registration, and startup policy.

Board examples provide three required narrow ports: display/input,
board-tested audio handles and AFE topology, and board services. An optional
storage port exposes an approved file root. Codec models, pins, panel
controllers, remote-Wi-Fi transport, and scheduler profiles remain outside
this component.

Storage is explicitly optional. A board that supplies a canonical file root
gets the bounded file-transfer commands and storage metrics; boards such as the
Waveshare adapter advertise no file surface.

`room_aec_src.c` carries the pinned `esp_capture` AEC/wake-event closure needed
by the current media stack: WakeNet only advances while its AFE fetch path is
drained. It lives here once, alongside the shared capture orchestration, until
the upstream capture component exposes the equivalent wake callback contract.
The closure retains Espressif's modified-MIT notice in
`LICENSE.ESPRESSIF-MODIFIED-MIT`; the rest of this component is Apache-2.0.
