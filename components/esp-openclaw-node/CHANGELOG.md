# This file contains the list of changes across different versions

## Unreleased

- Allow the Tab5 camera compatibility patch to discover standalone source trees across filesystem boundaries while still rejecting invalid Git metadata.
- Preserve teardown when queued disconnect or connect-failure completions run during destruction, preventing reconnect admission on a shutting-down node. Thanks @SebTardif. (#60)
- Keep directory pagination from repeating entries after unreadable file metadata is skipped.
- Keep room-node Canvas builds working with LVGL 9.6 by using its private-header umbrella instead of a moved internal header.
- Restore fresh Tab5 builds by pinning ESP Video to the 2.4.1 source required by the qualified CSI compatibility patch.
- Keep Talk SDP credentials on the direct offer endpoint by refusing HTTP redirects, and fail before sending if request headers or body cannot be configured. Thanks @SebTardif for reporting the redirect risk. (#32)
- Keep file parent creation inside the path when the storage root disappears, and allow side-effect-free write preflight beneath existing roots. Thanks @SebTardif for reporting the parent-walk overflow. (#31)
- Cancel room Talk on authoritative operator loss or exact Gateway session closure, and drain prepared-call callbacks before WebRTC teardown without changing public configuration layouts.
- Boost Tab5 and Waveshare playback with bounded PCM gain and tone compensation, preserve positional audio-port initializers, and bind room Talk calls to the configured agent. (#33)
- Update ESP-IDF component dependencies and the CI checkout action while preserving the vendored WebRTC stack and Tab5 firmware compatibility pins.
- Require Gateway-owned realtime control for room Talk and fail visibly before peer creation when the Gateway contract is unavailable.
- Add authoritative Gateway URI inspection and bound assembled inbound WebSocket messages to 2 MiB by default.

- Add `esp_openclaw_node_store_plugin_surface_url()` so applications can adopt refreshed capability-scoped surface URLs (for example from a `plugin.surface.refresh` RPC) into the component's canonical store.
- Point component manifest `url`/`repository` at the repo's new home under the `openclaw` GitHub org.
- Advertise gateway protocol `minProtocol 3` / `maxProtocol 4`. Current gateways strip plugin-owned capabilities (for example `canvas`) and withhold `pluginSurfaceUrls` from legacy v3-only node sessions.
- Capture `hello-ok` plugin surface URLs and expose `esp_openclaw_node_dup_plugin_surface_url()` for application URL resolution.
- Add role-keyed sessions, explicit device-token auth, operator scope advertisement, Gateway events, and correlated asynchronous RPCs for voice-room clients.

## v1.0.0

- Initial public release of the `esp-openclaw-node` ESP-IDF component.
