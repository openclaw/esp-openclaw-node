# This file contains the list of changes across different versions

## Unreleased

- Require Gateway-owned realtime control for room Talk and fail visibly before peer creation when the Gateway contract is unavailable.
- Add authoritative Gateway URI inspection and bound assembled inbound WebSocket messages to 2 MiB by default.

- Add `esp_openclaw_node_store_plugin_surface_url()` so applications can adopt refreshed capability-scoped surface URLs (for example from a `plugin.surface.refresh` RPC) into the component's canonical store.
- Point component manifest `url`/`repository` at the repo's new home under the `openclaw` GitHub org.
- Advertise gateway protocol `minProtocol 3` / `maxProtocol 4`. Current gateways strip plugin-owned capabilities (for example `canvas`) and withhold `pluginSurfaceUrls` from legacy v3-only node sessions.
- Capture `hello-ok` plugin surface URLs and expose `esp_openclaw_node_dup_plugin_surface_url()` for application URL resolution.
- Add role-keyed sessions, explicit device-token auth, operator scope advertisement, Gateway events, and correlated asynchronous RPCs for voice-room clients.

## v1.0.0

- Initial public release of the `esp-openclaw-node` ESP-IDF component.
