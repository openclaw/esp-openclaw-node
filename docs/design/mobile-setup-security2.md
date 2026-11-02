# Setup Security2 boundary

[`esp_openclaw_setup_security2`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_security2.h)
returns a public `protocomm_security_t` that delegates SRP6a, AES-GCM and opaque
handshake data to ESP-IDF Security2 version 2, patch 1. It does not start BLE,
generate proof of possession, attach/confirm a setup session, or expose an app
pairing entry. The [endpoint mailbox](mobile-setup-frames.md#sdk-endpoint-mailbox)
is implemented; server runtime, device screens and phone integration remain separate work.

The native implementation requires ESP32-S3 Wi-Fi and BLE, NimBLE, Security2 and
dynamic per-tag log control. Both remote Wi-Fi configurations and all other
targets return NULL with no SDK calls or peer authority. Watch and Dial defaults
enable the existing SDK options, including one NimBLE connection. Tab5 cannot
use this path to configure its remote C6 radio.

## Admission and authority

One setup server owns the SDK security handle. Its lifecycle and callbacks
serialize on the BLE host; drain callbacks before cleanup. Cross-task peer
queries and local revocation use a critical section. Never call SDK lifecycle
callbacks from the LVGL or setup owner task.

The guard reserves one transport ID **before** forwarding `new_transport_session`:
the SDK otherwise closes its old session unconditionally. A different connection
is refused without replacing or revoking the current peer. A repeated same-ID
new-session call, or a handshake request after authentication, revokes the peer
and requires close/reopen. A foreign close cannot revoke the current peer.

Handshake success or BLE CONNECTED grants no application authority. The first
successful SDK application decrypt creates a nonzero 64-bit peer ID. Its counter
has boot lifetime and survives server cleanup/reopen; exhaustion fails closed
instead of wrapping. Connection handles and BLE addresses are never peer IDs.
The serialized setup owner can use `peer_is_current` as its existing session
authority callback. Attach and fresh physical confirmation remain that owner's
responsibility; the guard cannot grant either.

Matching close, cleanup, local revoke, an attempted rehandshake, or SDK crypto
failure revokes synchronously. Call local revoke before awaiting server stop.
Before encrypting an endpoint reply, and again after the SDK returns, the guard
requires the same live authenticated peer. Thus an endpoint that waited for the
setup owner cannot deliver a reply after that authority was revoked. A failed
crypto call requires reconnect rather than guessing the SDK's nonce state.

## Buffer and secret lifetime

Application plaintext is 1–464 bytes; ciphertext is 17–480 bytes, including the
SDK's 16-byte GCM tag. The application [frame contract](mobile-setup-frames.md)
keeps the payload at 448 bytes. The guard never parses a private protobuf or
implements cryptography. Successful SDK output passes to protocomm; failed or
revoked SDK output and the guard's own state are wiped before release.

This does **not** guarantee erasure of SDK-private heap allocations. IDF's
protocomm owns and frees its decrypted request and endpoint plaintext reply;
the guard cannot erase those buffers after ownership has passed back. The
endpoint and server must wipe their own credential copies at their lifecycle
boundaries. The SDK shallow-copies salt/verifier pointers: keep their backing
allocations alive until protocomm security teardown, then wipe and free them.

Require `quiet_logs()` to return true **before** SRP parameter generation or
protocomm construction. Init repeats this check before SDK initialization. It
suppresses and reads back `security2`, `srp6a`, `protocomm`, `protocomm_nimble` and
`protocomm_ble`; failure prevents construction. Do not reenable these tags while
setup exists: the SDK's debug logs include secret material. No payload or secret
is logged by the guard. These SDK tags remain suppressed after teardown; the
guard never changes the global log level.

## Qualification

The [host runner](../../components/esp-openclaw-node-provisioning/tests/run_security2_host_tests.py)
compiles the actual guard against ESP-IDF's public protocomm headers. A bounded
vendor API fixture proves callback delegation, pre-SDK second-peer refusal,
first-decrypt authority, close/rehandshake/local revocation, revocation during
SDK encryption, buffer bounds and rejected-buffer wiping. A real setup session
consumes that authority and closes when it is revoked. Unsupported capability
profiles prove no SDK use. The fixture's bytes are not encrypted transport proof.

CI additionally compiles the native guard in the actual Watch/Dial builds and
checks its SDK reference, so silently compiling only the unsupported stub fails
the gate. The source audit uses IDF v5.5.5 commit
[`b774170ff46c393eeb5e495ea37936038d3f4f4f`](https://github.com/espressif/esp-idf/tree/b774170ff46c393eeb5e495ea37936038d3f4f4f).
Actual encrypted phone/device exchange, restart memory budgets and radio
coexistence require post-restart physical qualification.
