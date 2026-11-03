# Native phone-setup runtime

[`esp_openclaw_setup_runtime`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_runtime.h)
creates one boot-lifetime task after station/node initialization, with no advertising until local OPEN.
It reuses the canonical session, transfer, bindings, dispatcher, guard and mailbox. Native Watch/Dial
support requires controller modem sleep disabled (their existing default); remote-radio Tab5 and other
unsupported builds start no task. USB remains available. Device screens and phone integration are separate work.

## Owner and local controls

One four-entry queue carries OPEN, neutral input, physical confirmation and explicit retry. Its
nonwrapping local request ID covers queued OPEN/cancel and stale board action workers; generation/peer
still govern canonical authority. Queue rejection is visible and never replayed. Matching cancel
synchronously revokes and hides discovery without queue capacity: queued OPEN or the active 20 ms
owner poll already services cleanup. Closed/drained and stop-failed/drained states block indefinitely.
The [public action contract](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_runtime.h)
requires post-paint NEW physical released samples, then a fresh press for that comparison; remote menu
commands and synthesized LVGL clicks cannot confirm. Board integration owns painting/DMA/input proof.

The [armed mailbox route](mobile-setup-frames.md#sdk-endpoint-mailbox) returns PENDING on first decrypt;
only the owner creates one challenge, attaches the authenticated peer and binds the original deadline.
Hello borrows that same 32-byte challenge. Both screens show its first four bytes as uppercase hex in
byte order, separated after two bytes: `ab cd 12 34` becomes `ABCD 1234` (32-bit comparison, not PoP).
Physical confirmation activates leases once; explicit retry while READY completes partial activation.

## Discovery and secrets

Suppress/verify SDK logs before SRP or protocomm construction. The [native seed operation](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_node_wifi.h)
provides 32 fresh bytes while nonsleeping RF cannot be stopped by another owner operation. First 16
bytes yield a 128-bit PoP (32 lowercase hex); four separate bytes yield the public `PROV_OC_` suffix.
Wipe the seed on every path. SDK generates the public 16-byte salt/verifier; private handshake RNG runs
with active NimBLE and [controller modem sleep disabled](https://github.com/espressif/esp-idf/blob/b774170ff46c393eeb5e495ea37936038d3f4f4f/components/bt/controller/esp32c3/Kconfig.in#L354).

SDK-compatible QR: `{"ver":"v1","name":"PROV_OC_<8 hex>","username":"openclaw","pop":"<32 hex>","transport":"ble"}`.
Local snapshots expose manual service/PoP and QR only while discoverable; never log or capture secrets.
Service UUID is `1725244d-6b43-439b-877c-060f2d9bed07`; `prov-session=0xff51`, `proto-ver=0xff53`,
`oc-setup=0xff54`. Metadata is `prov.ver=v1.1`, `sec_ver=2`, `sec_patch_ver=1`, empty capabilities.
There is no standard `prov-config` or parallel `wifi_prov_mgr`: phones use the [custom protocol](mobile-setup-frames.md).

## Exit and qualification

Revoke before awaiting stop; close/wipe owned exchange/discovery. Public `protocomm_ble_stop` runs on
the owner outside locks with `keep_ble_on=false`. Success drains callbacks before protocomm deletion
and wiping/freeing salt/verifier. Failure retains revoked SDK/backing and requires explicit retry;
no restart or irreversible Bluetooth-memory release. SDK ignores controller-deinit return after stop,
so callback drain does not prove resource recovery. [SDK erasure limits](mobile-setup-security2.md#buffer-and-secret-lifetime) remain.
Keep dispatcher polling through native drain, block reopen until both leases release, and wake the
existing reconnect owner on Gateway handback. Wi-Fi recovery needs explicit retry; cancel implies no rollback.

Host proof runs the real task/public SDK boundary, mailbox/dispatch and native Wi-Fi with sanitizer
checks for failed construction, late cancel/expiry, hello/comparison identity, physical-action ordering,
queue pressure, partial activation, retained stop-failure backing, native drain, stale controls and idle.
Watch/Dial CI requires the runtime SRP reference. Fixture bytes do not prove encryption. Physical phone
exchange, repeated start/stop resources, coexistence and device heap/stack headroom await Studio qualification.
