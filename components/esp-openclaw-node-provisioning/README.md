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
the confirmed setup bindings also reserve exclusive phone-setup ownership.

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

`esp_openclaw_node_wifi_setup_seed()` fills 32 bytes for native setup bootstrap
under the existing station mutex. It requires a running driver, no active radio
operation and a current `WIFI_PS_NONE` result. It preserves the link, configuration
and reconnect policy, and acquires no setup lease. Down, busy, sleeping or remote
radios fail without changing the output. The caller must wipe this seed after
deriving its ephemeral secret; this API does not start BLE or provide pairing.
The RF requirement follows the pinned
[ESP-IDF RNG contract](https://github.com/espressif/esp-idf/blob/b774170ff46c393eeb5e495ea37936038d3f4f4f/docs/en/api-reference/system/random.rst).

`esp_openclaw_setup_runtime.h` owns the native setup task and copied local
snapshots. Read its [runtime contract](../../docs/design/mobile-setup-runtime.md)
before integrating a board. Device screens and the phone adapter remain separate work.

`esp_openclaw_setup_frame.h` provides the allocation-free
[setup frame codec](../../docs/design/mobile-setup-frames.md). It validates the
shared byte envelope and bounded fragment sizes; it does not authenticate a
peer, reassemble a transfer, or expose a BLE endpoint.

`esp_openclaw_setup_transfer.h` assembles those frames on the serialized setup
owner. Initialize its caller-owned storage once per boot, outside small task
stacks, and bind it after the session attaches an authenticated peer. It reserves
two fixed 8 KiB buffers: the original request remains available for retry checks
while a separate response is retained for repeatable pulls. Only one transfer
can be pending; increasing IDs replace a completed transfer and wipe both buffers.
Close also wipes them, and the same setup generation cannot reopen the transfer
owner. Poll it even when no frames arrive to detect revocation and expiry.

`receive` returns an encoded reply and hands off a complete request exactly once.
Copy the borrowed request before asynchronous work or retiring the transfer.
Decode and authorize application operations separately; an ACK is not operation
success. `complete` accepts only the current pending generation/peer/transfer
token. Empty or oversized responses become stable transport errors, not partial
responses. Before local confirmation, peer authorization permits comparison
messages only; it does not enable mutations or saved-state reads.

`esp_openclaw_setup_request.h` decodes the shared
[application request format](../../docs/design/mobile-setup-requests.md) without
allocation. It preserves full-width generation/operation IDs and returns bounded,
borrowed SSID/password/setup-code spans. The caller still owns live authorization,
confirmation, C-string adaptation, and dispatch through the existing bindings.
`esp_openclaw_setup_response.h` encodes the bounded
[application response format](../../docs/design/mobile-setup-responses.md), with
explicit public wire codes and separate admission, outcome and observed-effect
fields. It neither generates a comparison challenge nor maps native errors or
authorizes access. The endpoint mailbox and shared native runtime use these codecs.

`esp_openclaw_setup_dispatcher.h` routes all six application operations through
these owners. Initialize it once with the session, transfer, Wi-Fi/Gateway
bindings and a callback borrowing the same encrypted-peer comparison shown by
the local UI. It owns only bounded scratch, including 33/65/4097-byte C-string
adaptations; keep it separate from transfer storage and off small task stacks.
The dispatcher wipes its C-string scratch after the native owner synchronously
copies it. The transfer retains the original request under its existing bounded
retry and buffer-wiping lifecycle.

After local confirmation, call `dispatcher_activate` once to acquire the existing
leases without changing either link. If activation partly fails, retain the
successful lease and retry while READY or close and keep polling. Do not activate
on every request: retries must still work while an operation is BUSY.
Pass each complete transfer handoff to `dispatcher_handle` on the owner task.
It checks the exact pending token, buffer and length before decoding or effects,
then completes the transfer immediately with an admission/error response.
Read terminal receipts with new status transfers. The session retains the sole
32-receipt deduplication authority, including native payload rejection.

Before confirmation, only hello and cancel are admitted. Status requires READY
or BUSY; scans require idle READY and delegate to the Wi-Fi binding's private
lease. A connected/reconnecting radio refuses scans without disconnecting;
offer manual SSID entry or Other ways. Blocking scans recheck peer authority
before results can be published. Tab5 remote-radio joins remain unsupported.

Forward the session cancel callback to `dispatcher_cancel`. Cancellation closes
reply authority and does not promise a response or rollback. Call
`dispatcher_poll` regularly even after BLE or the setup screen closes; it polls
both bindings even when one reports an error. A binding polled before the other
closes the session may release its lease on the next tick. Wake the existing
automatic Gateway reconnect owner when `gateway_released` is true.

Response outcome/category comes from the exact canonical receipt. Native
progress/save observations are included only for a matching generation and
operation ID with an observed native attempt; older/missing observations remain
unknown. A false save observation never promises old credentials survived.
The integrated Wi-Fi host lane exercises real frames, transfers, request/response
codecs, dispatcher, both bindings and the native Wi-Fi owner. Its Gateway API
boundary fixture complements the existing real connection-worker/lease tests;
neither proves BLE authentication or phone pairing.

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
UI metadata. Receipt details are local native failure values, never secrets;
map them to stable public response categories before sending over transport.

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
