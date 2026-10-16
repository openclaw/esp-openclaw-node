# This file contains the list of changes across different versions

## Unreleased

- Reserve the AFE worker's internal stack for the audio source lifetime so repeated Talk transitions cannot lose microphone capture to fragmented RAM.
- Clarify voice-node pairing for room/watch profiles and document live Talk verification without assuming an automatic greeting.
- Reduce watch display DMA buffers to leave internal RAM for Talk capture transitions and both Gateway connections; include the error code when AFE initialization fails.

## 1.2.0 - 2026-10-16

**Highlights:** New Waveshare watch and M5Dial profiles, USB Wi-Fi discovery, reliable dial gestures, and resilient connection recovery.

- Add a Waveshare 1.8-inch watch profile with CO5300/CST816S display support and shared-ES8311 microphone/speaker lifecycles for the full room-node feature set.
- Add an M5Dial example with touch/rotary controls, session-bound agent actions, an animated face, text display, and bounded buzzer playback.
- Add a bounded USB `wifi scan` command for provisioning disconnected nodes without changing saved credentials.
- Keep the M5Dial encoder editable after confirmation, recognize stationary button holds, and prevent touch drags from submitting actions or returning home.
- Retry saved-session requests rejected before admission so temporary memory pressure cannot strand a node waiting for an event that will never arrive.
- Preserve the 12-second handshake deadline through transport startup and check it while draining queued messages so stalled connections can retry. Thanks @DonnieFi. (#67)
- Update the registry WebSocket client to 1.8.0 for upstream stop/destroy race, error-handling, and close-state fixes.

Qualification scope: host checks and CI firmware builds only. Publication requires all seven firmware builds to pass on the release commit. No hardware flashing, on-device Unity execution, live Gateway pairing/reconnect, acoustic wake/Talk, display/Canvas, camera/DMA, SD-card removal, or C6 coprocessor qualification is claimed for this release.

## 1.1.1 - 2026-10-01

**Highlights:** Refreshed room-node speech dependencies, a stable CI toolchain, and reliable component publication verification.

- Update both room examples to ESP-SR 2.5.2 and pin their CI toolchain to ESP-IDF 5.5.5 while preserving the qualified Tab5 SDK image. Thanks @steipete. (#34)
- Allow bounded registry propagation time during post-upload artifact verification instead of failing immediately after a successful component upload.

Qualification scope: host checks and CI firmware builds only. Publication requires all five firmware builds to pass on the release commit. No hardware flashing, on-device Unity execution, live Gateway pairing/reconnect, acoustic wake/Talk, display/Canvas, camera/DMA, SD-card removal, or C6 coprocessor qualification is claimed for this release.

## 1.1.0 - 2026-09-24

**Highlights:** Safer teardown and Talk signaling, role-aware Gateway sessions, refreshed surface URLs, and reliable room-node builds.

- Keep Talk SDP credentials on the direct offer endpoint by refusing HTTP redirects, and fail before sending if request headers or body cannot be configured. Thanks @SebTardif for reporting the redirect risk. (#32)
- Preserve teardown when queued disconnect or connect-failure completions run during destruction, preventing reconnect admission on a shutting-down node. Thanks @SebTardif. (#60)
- Keep file parent creation inside the path when the storage root disappears, and allow side-effect-free write preflight beneath existing roots. Thanks @SebTardif for reporting the parent-walk overflow. (#31)
- Add role-keyed sessions, explicit device-token auth, operator scope advertisement, Gateway events, and correlated asynchronous RPCs for voice-room clients.
- Advertise gateway protocol `minProtocol 3` / `maxProtocol 4`. Current gateways strip plugin-owned capabilities (for example `canvas`) and withhold `pluginSurfaceUrls` from legacy v3-only node sessions.
- Capture `hello-ok` plugin surface URLs and expose `esp_openclaw_node_dup_plugin_surface_url()` for application URL resolution.
- Add `esp_openclaw_node_store_plugin_surface_url()` so applications can adopt refreshed capability-scoped surface URLs (for example from a `plugin.surface.refresh` RPC) into the component's canonical store.
- Add authoritative Gateway URI inspection and bound assembled inbound WebSocket messages to 2 MiB by default.
- Cancel room Talk on authoritative operator loss or exact Gateway session closure, and drain prepared-call callbacks before WebRTC teardown without changing public configuration layouts.
- Require Gateway-owned realtime control for room Talk and fail visibly before peer creation when the Gateway contract is unavailable.
- Keep directory pagination from repeating entries after unreadable file metadata is skipped.
- Keep room-node Canvas builds working with LVGL 9.6 by using its private-header umbrella instead of a moved internal header.
- Restore fresh Tab5 builds by pinning ESP Video to the 2.4.1 source required by the qualified CSI compatibility patch.
- Allow the Tab5 camera compatibility patch to discover standalone source trees across filesystem boundaries while still rejecting invalid Git metadata.
- Boost Tab5 and Waveshare playback with bounded PCM gain and tone compensation, preserve positional audio-port initializers, and bind room Talk calls to the configured agent. (#33)
- Update ESP-IDF component dependencies and the CI checkout action while preserving the vendored WebRTC stack and Tab5 firmware compatibility pins.
- Point component manifest `url`/`repository` at the repo's new home under the `openclaw` GitHub org.

Qualification: host checks and all five CI firmware builds passed. No hardware flashing, on-device Unity execution, live Gateway pairing/reconnect, acoustic wake/Talk, display/Canvas, camera/DMA, SD-card removal, or C6 coprocessor qualification was performed for this release.

## v1.0.0

- Initial public release of the `esp-openclaw-node` ESP-IDF component.
