# Mobile setup for Watch and Dial

**Implementation proposal. No BLE pairing or new mobile support ships with this document.**

Both devices should complete setup through the same phone journey: scan the
device, confirm the nearby device, choose Wi-Fi, authorize OpenClaw, and choose
an agent. Each then works independently over Wi-Fi. USB setup remains available.
This contract turns the [interaction plan](watch-dial-interaction-plan.md#mobile-pairing-qr-discovery-and-bluetooth-setup)
into bounded implementation work; it does not make its simulated screens live.

## Existing owners

Source baseline: ESP commit `2a27a66a597b525ab0a651a504b6c90f5c00f6f2`.
The linked Gateway/mobile source was inspected at OpenClaw commit
`a3fee9204353615d4b8d3bea7a94504ff9bd5fc3`; it is a source baseline, not a
minimum supported release.

| Boundary | Existing behavior | Reuse or required change |
| --- | --- | --- |
| [Wi-Fi owner](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_wifi.c) | Loads ESP-IDF station credentials, owns reconnect policy, validates credential lengths, exposes bounded scans | Add exclusive setup ownership here; do not run a second connection policy beside it |
| [Node requests](../../components/esp-openclaw-node/include/esp_openclaw_node.h) | `SETUP_CODE` submits an asynchronous connect request; saved-session reconnect is separate | Reuse this request path; accepted is not connected |
| [Node lifecycle](../../components/esp-openclaw-node/src/esp_openclaw_node_runtime.c) | Explicit connect can replace a live attempt; terminal events have no caller request ID | Correlate setup attempts and drain cancellation before admitting a replacement |
| [USB console](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_example_repl_cmd.c) | Existing Wi-Fi and setup-code entry | Keep available; a conflicting setup write must pass through the same owner |
| [Gateway setup code](https://github.com/openclaw/openclaw/blob/a3fee9204353615d4b8d3bea7a94504ff9bd5fc3/src/gateway/server-methods/device-pair-setup.ts) | `device.pair.setupCode` selects a bootstrap profile and returns a setup code | Request a fresh code under the phone's current authority; do not copy its identity or saved token |
| [Mobile QR parser](https://github.com/openclaw/openclaw/blob/a3fee9204353615d4b8d3bea7a94504ff9bd5fc3/apps/shared/OpenClawKit/Sources/OpenClawKit/DeepLinks.swift) | Scans Gateway connection material for the phone itself | Device discovery needs a separate destination; preserve the existing Gateway scanner |

The shipping mobile app and its owning checkout must be identified before the
mobile PR. The existing OpenClaw iOS code is an integration reference, not proof
that another mobile app already has these APIs or permissions.

## Transport decision

Use ESP-IDF `protocomm` BLE with Security 2 and a small OpenClaw setup endpoint.
Keep vendor transport and cryptography in an adapter. Keep setup state, input
confirmation, connection ownership, and UI snapshots outside that adapter.

| Option | Fit | Decision |
| --- | --- | --- |
| Full `wifi_prov_mgr` plus custom Gateway endpoint | Provides standard network scanning/configuration, but also changes station configuration and manages connection attempts | Requires handing Wi-Fi ownership to the manager and reconciling it afterward; defer this larger lifecycle change |
| `protocomm` BLE plus existing Wi-Fi/node owners | Reuses encrypted sessions and mobile custom-endpoint support; needs a bounded application request contract | Recommended; one owner continues to apply credentials and run reconnects |
| Browser SoftAP | Useful for clients without BLE; requires another transport and phone network switching | Later adapter to the same setup owner, not a separate setup implementation |

[Security 2](https://github.com/espressif/esp-idf/blob/v5.5.5/components/wifi_provisioning/include/wifi_provisioning/manager.h)
uses SRP6a and AES-GCM. Require it on both ends; reject unsupported versions
instead of falling back to plaintext, absent proof of possession, or Security 1.
Use the SDK cryptography rather than implementing a key exchange in application code.

Generate fresh high-entropy proof-of-possession material when the person opens
setup. Keep it and its salt/verifier in RAM. ESP-IDF provides
[`esp_srp_gen_salt_verifier`](https://github.com/espressif/esp-idf/blob/v5.5.5/components/protocomm/include/crypto/srp6a/esp_srp.h)
for display devices that generate a different password each time. Never use the
example password, a MAC-derived password, or a six-digit confirmation code as
the transport secret. Measure generation cost on both boards before choosing
the setup task's stack and memory budgets.

Do not unconditionally release BLE memory after setup: the user must be able to
open setup again without rebooting. Stop advertising and close the temporary
session when leaving setup; qualify stop/start and memory reclamation on both boards.

## Discovery and physical confirmation

The device QR carries only the discovery version, service name, transport,
Security 2 username and ephemeral proof of possession. It carries no Wi-Fi
password, Gateway credential, saved node token, or conversation identity.
Use Espressif's supported discovery shape where possible. Nearby discovery
uses the same transport; manual entry of the displayed secret replaces scanning.
Nearby selection alone is not authentication.

Generate the QR from the current setup generation. Reopening setup rotates it.
Do not route it into the phone's existing Gateway-connection scanner. Render a
full quiet zone and integer-sized modules; measure the actual payload before
choosing tile size. The Dial's round screen needs camera proof at physical size.
Keep manual entry available if the payload cannot be scanned reliably.

After the encrypted session is established, exchange a fresh random challenge
inside it and show its derived comparison on both displays. This is an
application confirmation bound to that authenticated session, not a second
key exchange. The person confirms on the Watch or Dial. No Wi-Fi or Gateway
write is accepted before this local event. A remote `confirm` request cannot
stand in for physical input.

Bind the confirmation to the setup generation and transport session ID.
Revalidate both immediately before each side effect, including after queued or
awaited work. A new peer, reconnect, timeout, cancellation, or setup exit clears
confirmation. One active peer may own setup. A second connection cannot replace
the confirmed peer or its display. A held gesture from before the confirmation
screen must be released before confirmation can occur.

## Setup owner and request contract

Use one serialized setup owner on the device. Its RAM state contains the current
generation, authenticated peer, local confirmation, operation ID, and outcome.
BLE callbacks copy bounded requests into this owner and return promptly; they
do not change LVGL objects, persist credentials, or wait for a network connection.
UI and mobile receive snapshots from this owner.

Proposed operations for the encrypted custom endpoint:

| Operation | Allowed state and result |
| --- | --- |
| `hello` | Returns device label/model, supported setup version, and comparison challenge; no saved network or account details before confirmation |
| `status` | Returns the current generation and operation outcome for this peer; never returns secrets |
| `wifi.scan` | Confirmed setup owner only; uses the existing scan limit of 16 and returns truncation explicitly |
| `wifi.apply` | Confirmed owner, explicit phone submission, validated original SSID/password; starts one connection attempt |
| `gateway.apply` | Confirmed owner with Wi-Fi ready; validates the setup code and displayed destination, then starts one node connect request |
| `cancel` | Cancels pending setup work owned by this peer, starts teardown, and reports the effect of any already-applied change |

Version the application envelope separately from the Gateway protocol. The
first implementation must specify and test exact wire fields and error codes
before the transport PR. Initial resource limits: one active operation, at most
16 scan results, an 8 KiB request frame, and a 4 KiB decoded setup-code field.
Reject oversize, malformed, unknown-operation, and unsupported-version input
before allocation or mutation. Confirm these proposed bounds against real
setup-code fixtures; do not silently truncate valid credentials or identities.

The shared [frame codec](mobile-setup-frames.md) defines the binary fragment
envelope within the pinned BLE attribute limits. Transfer reassembly and the
application-operation schema remain separate work; an 8 KiB logical request
must never be sent as one characteristic value.

Every mutating request has an operation ID and expected setup generation. Once
accepted, a repeated ID returns that operation's status; it cannot run again or
change its payload. Retain at most 32 mutating-operation receipts per setup
generation. When full, require fresh setup confirmation before accepting more
writes; never evict receipts and then execute an old ID again.
After a lost acknowledgment, query status. Do not automatically submit a new
operation ID. Clear receipts when closing setup; a reconnect requires fresh
local confirmation and cannot replay a prior generation.

The Wi-Fi owner must provide an exclusive setup lease that suppresses its
ordinary reconnect loop and rejects conflicting console writes. Release the
lease only after in-flight work has stopped. Adapt its existing scan, apply,
status and reconnect paths rather than duplicating `esp_wifi_*` mutations in BLE.

The current Wi-Fi API implements admission ownership through
`esp_openclaw_node_wifi_setup_acquire`, `setup_scan`, and `setup_release`. A
boot-unique lease excludes ordinary mutations and scans; automatic reconnect
requests wait until release. Status remains readable and the existing link is
preserved. A blocking scan must return before release. The release return value
describes ownership, while its output reports any immediate reconnect error.
Acquisition alone does not authenticate a phone or drain an earlier join.

Native-radio adapters can use `setup_join_begin`, `setup_join_poll`,
`setup_join_cancel`, and `setup_join_retry_drain` under that lease. One serialized
owner task polls the attempt outside the default event loop and supplies a live
authorization callback. Before applying credentials, the owner stops and
deinitializes the radio, then crosses two event-loop barriers: an older Wi-Fi
handler can itself enqueue an IP event behind the first barrier. Success needs
the new attempt's association and IP on the station netif. Disconnect is latched;
a later IP event cannot turn that failed attempt into success.

Cancellation and the 30-second join deadline retain ownership until the same
radio/event drain completes. A failed stop, deinit, barrier, or restart reports
`RECOVERY_REQUIRED`; polling alone does not retry it or release ownership. An
explicit drain retry uses a fresh generation. Cancellation before work starts
preserves the current link. After work starts, saved credentials remain, while
automatic reconnect stays disabled after failure or cancellation; an explicit
Connect action can resume it. A successful attempt hands its link back to the
ordinary Wi-Fi owner.

Remote-radio backends return `ESP_ERR_NOT_SUPPORTED` before mutation. The Tab5's
host event queue does not prove its coprocessor has drained. This native API is
host-tested, not yet physically qualified or connected to BLE/mobile setup.
`wifi.apply` and setup-session completion must use its terminal result rather
than treating admission, a saved configuration, or `RECOVERY_REQUIRED` as success.

The transport-free [`esp_openclaw_setup_wifi` binding](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_wifi.h)
connects these attempts to the confirmed setup session. Activate it after local
confirmation, then submit `WIFI_APPLY` through the binding so the session owns
peer checks and duplicate receipts. Poll it on the same owner task, including
after closing setup. It keeps the Wi-Fi lease between operations and releases
it when the session has closed and its pending attempt has drained.

Forward the session's cancellation callback to `esp_openclaw_setup_wifi_cancel`;
that callback only sets a matching cancellation flag. Session expiry can invoke
it from inside the Wi-Fi authorizer, where reentering radio APIs is forbidden.
Only terminal attempts complete receipts. A drained cancellation is `FAILED`
with `ESP_OPENCLAW_SETUP_WIFI_CANCELLED`; it does not mean credentials were
rolled back. `RECOVERY_REQUIRED` keeps the operation pending, and the binding's
explicit drain retry remains available while the session is closing. Its local
snapshot retains the final attempt even after closing clears session receipts.
No BLE transport or device setup screen invokes this binding yet.

Scanning a connected station requires an explicit, confirmed network-repair
transition; do not silently disconnect it just because the phone opens a list.
Preserve exact SSID bytes supported by the existing API; display names and
ellipsized rows are never operation targets.

The transport-free [`esp_openclaw_setup_gateway` binding](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_gateway.h)
connects `GATEWAY_APPLY` to a node control lease and an exact native attempt ID.
It uses `connect_begin_authorized` to check the current peer and setup deadline
on the node worker before connection and token-write effects. The binding
captures immutable operation metadata; the worker never reads the mutable
setup session. Clock and live-peer callbacks must be bounded and thread-safe
on both tasks, independent of queued UI notifications.

Forward matching session cancellation to the binding and poll it through
`CLOSING`. It completes receipts only from the exact terminal native result,
retains earlier saved-token effects, and releases the lease after setup closes.
When poll reports release, wake the application's automatic reconnect owner;
release alone does not schedule it. A completed connection racing setup exit
remains connected. No BLE endpoint or device screen invokes this binding yet.
The transport/UI still own bounded payload decoding and destination confirmation;
generic `CONNECTED` events must not substitute for a correlated result.

## Visible progress and recovery

| Device/phone state | Meaning and next action |
| --- | --- |
| Ready to scan | Ephemeral discovery is active; Other ways offers existing USB setup |
| Confirm device | Encrypted peer is connected; physical confirmation is still required |
| Choose Wi-Fi | Confirmation succeeded; network selection remains a draft |
| Joining Wi-Fi | One accepted attempt is running; no second Apply action |
| Wi-Fi failed | Explain authentication, missing network, or timeout; allow an explicit corrected retry |
| Connect OpenClaw | Wi-Fi is ready; show the destination and requested access before submission |
| Connecting | Node request accepted; wait for its correlated terminal event |
| Approval needed | Show only when the real Gateway response reports pending approval |
| Ready | Current node attempt is authenticated; choose an agent, then close setup |

Cancellation is not rollback. Before a write, it discards the draft. After Wi-Fi
credentials are saved, report that the network remains saved. After Gateway
authorization, closing setup does not revoke the device's remote grant.
Network repair keeps Gateway identity and selected agent; Gateway replacement
keeps Wi-Fi. Explain these consequences before changing an existing connection.
Do not erase unrelated NVS or promise to restore the old network automatically.

After phone disconnect, timeout, screen exit, or reboot, stop accepting writes,
cancel queued operations, clear secrets/confirmation and show the observed
connection state. Completed Wi-Fi progress survives through its existing store;
the next setup resumes the missing step after fresh authentication. An unknown
outcome stays unknown until the owning connection state resolves it.

## Mobile adapter

Espressif's [iOS SDK](https://github.com/espressif/esp-idf-provisioning-ios/blob/78e58f010f8539f800bce063a26d3e4c4ac596b9/ESPProvision/ESPDevice.swift)
supports encrypted custom endpoints through `sendData`. Wrap that seam; the
chosen design does not call its standard `provision` operation, which would
bypass OpenClaw's Wi-Fi owner. Keep mobile discovery/session work separate from
the existing Gateway login and node identity.

The inspected SDK logs include proof-of-possession and Wi-Fi credential
arguments. Verify an effective logging-off policy before constructing an SDK
device; if the pinned SDK cannot suppress those calls, resolve that dependency
issue before adoption. App logs, analytics, error reports, screenshots, and
test fixtures must not contain QR secrets, passwords, or bootstrap tokens.
Sanitize QR/manual-secret regions before sharing device qualification captures.

Request camera and Bluetooth access when the person chooses those routes.
Camera denial offers Nearby plus manual secret entry. Bluetooth denial explains
the OS setting and offers USB. Neither failure changes Gateway login or device
configuration. Cancel and background handling invalidate the mobile operation
generation so late callbacks cannot restart discovery or submit a draft.

## Narrow PR sequence and proof

1. **Setup owner:** typed state/operations and exclusive admission, independent
   of transport. Prove local confirmation, generation changes, duplicate IDs,
   bounds, and cancellation ordering against the production owner.
2. **Wi-Fi/node lifecycle:** integrate existing owners and correlated outcomes.
   Prove failed joins, stale success, conflicting console writes, disconnects,
   repair preservation, and reboot behavior. Keep USB working.
3. **BLE adapter:** Security 2, ephemeral discovery, strict endpoint decoding,
   start/stop and resource cleanup. Exercise an actual encrypted client session,
   wrong proof, tampering, peer replacement, replay and malformed input.
4. **Mobile adapter:** pin/review the SDK, suppress secret logging, route device
   QR separately, and implement explicit request/status handling. Prove real
   BLE connection and permission/background failures before adding a public entry.
5. **Watch setup:** connect the actual owner to QR, physical confirmation,
   network and Gateway progress, outcomes, Back and Other ways. Inspect native
   captures and prove wake/held-input behavior; then flash and record the Watch.
6. **Dial setup:** use the same states with encoder/touch navigation. Prove
   round clipping, readable QR/manual secret, explicit confirmation and Back;
   then flash and record the Dial.
7. **End-to-end qualification:** set up each device independently with the phone,
   restart it, disconnect the phone, and use agents/conversations/controls.
   Repeat with wrong password, denied access, expired code, interrupted BLE,
   and repair of only one connection. Record both displays and real outcomes.

The UI entry ships only when the underlying flow works. A rendered QR, native
input test, firmware build, or mocked BLE response does not prove phone pairing.
Physical devices/camera and mobile-app integration remain unverified at this
baseline. These are required qualification steps, not completed capabilities.
