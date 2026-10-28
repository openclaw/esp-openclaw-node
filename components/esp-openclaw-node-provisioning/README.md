# esp-openclaw-node-provisioning

Reusable local provisioning for ESP OpenClaw examples: NVS-backed station
credentials, the USB REPL, setup-code commands, saved-session reconnect, and
the canonical device/Wi-Fi status handlers. Network hardware must be prepared
by the board before `esp_openclaw_node_wifi_start()` is called; this is what
allows the same station helper to run over native Wi-Fi or `esp_wifi_remote`.

Wi-Fi credential changes, connect/disconnect commands, and scans admit one
explicit operation at a time. A competing call returns `ESP_ERR_WIFI_STATE`
before changing station configuration or reconnect policy. Status remains
readable while an operation runs. This serializes existing command calls;
exclusive phone-setup ownership is a separate integration step.

Use `wifi scan` in the USB console before setting credentials to list up to 16
nearby networks, strongest first, with channel, RSSI, and the ESP-IDF auth mode.
It leaves credentials unchanged and refuses to scan while connected or
reconnecting. Run `wifi disconnect` first if needed, then `wifi connect` to
resume using saved credentials. Hidden networks are omitted; enter their SSID
directly with `wifi set`.

`wifi.status` reports connection state independently from access-point readings.
While connected, `rssi` and `channel` are `null` and `authMode` is `"unknown"`
if the driver cannot read the current access point. They are omitted while
disconnected. C consumers must check `ap_info_valid` before using RSSI, channel,
or auth mode; zero is not an unavailable-reading sentinel. Room diagnostics
display unavailable RSSI as `unknown`.

## Shared setup session owner

`esp_openclaw_setup_session.h` provides transport-independent setup admission
for Watch and Dial. It is a library foundation; no example exposes BLE setup
or a new pairing screen yet. Wi-Fi/node lifecycle integration and the encrypted
transport follow the [mobile setup contract](../../docs/design/mobile-provisioning-contract.md).

Initialize one session per boot. Serialize its API, UI events, transport events,
and side effects on one owner task. `open` creates a new generation with a
two-minute lifetime. `attach` binds one currently authenticated encrypted peer;
a second peer cannot replace it. Display that peer's comparison before sending
`input_released`, then call `confirm` only for a new local physical press. These
two calls are local-only, never endpoint operations.

`submit` admits one Wi-Fi or Gateway mutation at a time and dispatches immediately.
The dispatcher must validate the operation payload and connection prerequisites
through their canonical owners. It copies any payload needed by asynchronous
work; the session holds no credential strings. After every queue or wait, call
`authorize_operation` on the owner task immediately before applying a side effect.
It checks the current authenticated peer and deadline, not just the token. The
live-peer callback must track transport revocation independently of queued UI
notifications. Neither a stored peer ID nor a previously valid handle suffices.

An accepted operation gets a receipt before dispatch. Acceptance does not imply
connection or persistence, and the returned receipt is the admission snapshot;
use `get_receipt` for later results, including synchronous dispatch failure.
Repeated IDs return the original receipt without redispatch or payload replacement.
Reusing an ID with a different operation is invalid. Keep IDs nonzero and unique
within a generation. At 32 receipts, close and reopen setup; nothing is evicted.

Local `close`, matching peer loss (`detach`), or expiry revokes admission immediately.
Old-generation or other-peer disconnect events cannot close the current session.
Pending work enters
`CLOSING` and receives one cancel callback. That callback requests cancellation
and draining, not rollback. Report `complete` only when no late side effects remain;
until then, reopening is refused. A stale generation cannot complete a newer
attempt. Use `UNKNOWN` when work is drained but its external outcome is unresolved.
Do not infer whether credentials remain saved from a canceled receipt; consult
the Wi-Fi/node owner. Poll regularly for expiry, and use `snapshot` only for local
UI metadata. Receipt details must be numeric public error codes, never secrets.

Clock and live-peer callbacks must not reenter the owner. Dispatch and cancel may
synchronously call `complete` (for queue rejection or completed draining), but
must not otherwise reenter. Keep the session and callback context alive until all
accepted operations have drained. Do not reinitialize it to escape `CLOSING`.

## Confirmed Gateway setup binding

`esp_openclaw_setup_gateway.h` connects `GATEWAY_APPLY` to the native node
connection owner. It does not expose a BLE endpoint or a device setup screen.
Initialize it once with the setup session and node, then activate it after
local confirmation. Activation reserves connection control while preserving
the current link. Submit a setup code through the binding; the native owner
validates and copies it, and Wi-Fi must already have a live IP. Admission is
not connection success. Duplicate operation IDs return their existing receipt.

Forward the session cancellation callback to `esp_openclaw_setup_gateway_cancel`
for its Gateway operation, and poll the binding on the serialized setup owner,
including during `CLOSING`. Cancellation uses the native exact-request flag,
independent of queue capacity. Only a terminal native snapshot completes a
receipt. A cancelled request is `FAILED` with
`ESP_OPENCLAW_SETUP_GATEWAY_CANCELLED`. Earlier saved tokens remain visible in
the binding's final snapshot; a successful connection that races setup exit is
left connected. Polling API/ownership errors keep the operation pending and the lease held.

The native worker rechecks the captured peer and deadline before connection
and credential effects. The session's clock and live-peer callbacks must be
bounded and thread-safe on both tasks. They must consult current transport
revocation independently of queued UI messages, and must not access mutable
setup/UI state or call node/session APIs. The worker never reads the mutable
setup session. Keep the session, node, binding and callback context alive until
the operation has drained and its lease has been released.

Poll reports `released = true` exactly when it releases the node lease after
setup closes. Wake the application's automatic reconnect owner then; lease
release does not itself schedule a reconnect. Subsequent polls report false.
The local snapshot retains the final request and `credentials_saved` after
session close clears its receipts. The transport and UI still own payload bounds,
destination display/confirmation, and safe handling of setup-code secrets.
