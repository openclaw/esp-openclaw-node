# Mobile setup frames

The Watch and Dial share this version 1 byte format for the encrypted custom
setup endpoint. The structural codec and transfer owner are implemented; BLE
transport, application dispatch, and mobile integration are not connected yet.
The [application request codec](mobile-setup-requests.md) decodes completed requests;
response messages remain separate work.
See the [setup contract](mobile-provisioning-contract.md) for local confirmation
and operation ownership. A valid frame does not authorize a setup operation.

## Transport budget

ESP-IDF v5.5.5's [Bluedroid transport](https://github.com/espressif/esp-idf/blob/v5.5.5/components/protocomm/src/transports/protocomm_ble.c)
limits a Security 2 characteristic value and prepared-write buffer to 481 bytes.
Its [NimBLE transport](https://github.com/espressif/esp-idf/blob/v5.5.5/components/protocomm/src/transports/protocomm_nimble.c)
uses a 512-byte attribute limit. [Security 2](https://github.com/espressif/esp-idf/blob/v5.5.5/components/protocomm/src/security/security2.c)
adds a 16-byte AES-GCM tag. Use a 16-byte header and at most 448 payload bytes:
464 bytes before encryption, 480 afterward. Do not send the logical 8 KiB
message in one characteristic write or enlarge SDK buffers.

This budget is for a complete attribute value. The eventual client must use
the SDK's supported long-write/read behavior when the negotiated ATT MTU is
smaller; this codec does not implement ATT fragmentation. Qualify that behavior
with the pinned mobile SDK and both physical boards before enabling pairing.

## Header and shapes

All integers are unsigned and big-endian. There is no alignment or native C
struct on the wire. The exact frame length is 16 plus `payload_size`; no trailing
bytes are accepted. Reserved bytes must be zero. Unknown versions, kinds, and
error codes are rejected.

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 2 | Magic: ASCII `OC` (`4f 43`) |
| 2 | 1 | Version: `01` |
| 3 | 1 | Kind, below |
| 4 | 4 | Nonzero transfer ID |
| 8 | 2 | Total logical message size |
| 10 | 2 | Byte offset |
| 12 | 2 | Payload size, at most 448 |
| 14 | 2 | Reserved: `00 00` |
| 16 | variable | Payload |

| Kind | Value | Total | Offset | Payload |
| --- | --- | --- | --- | --- |
| Request fragment | `01` | 1–8192 | Within total | 1–448 bytes, ending at or before total |
| Pull response | `02` | 0 | 0–8191 | Empty |
| Request acknowledgment | `81` | Original request total | Next expected request offset, at most total | Empty |
| Response fragment | `82` | 1–8192 | Within total | 1–448 bytes, ending at or before total |
| Transport error | `ff` | 0 | 0 | Exactly one two-byte error code |

Request and response sizes are independent. The same transfer ID correlates
the request, acknowledgment, response pulls, and response fragments. Transfer
IDs are not setup generations or mutating-operation IDs.

| Error code | Meaning | Client action |
| --- | --- | --- |
| 1 | Invalid frame or transfer sequence | Correct the frame; do not advance the offset |
| 2 | Another transfer owns the buffer | Finish that transfer before starting another |
| 3 | Transfer is no longer available | Query the application receipt with a new transfer after confirming the session is current |
| 4 | Same transfer ID contains conflicting bytes or metadata | Close setup; never guess which mutation ran |
| 5 | Response is pending | Pull again after a bounded delay; do not resubmit the mutation |
| 6 | Logical request or response exceeds its limit | Surface the limit; never truncate |
| 7 | Setup authority is no longer current | Reopen setup and confirm locally |
| 8 | Transfer failed internally | Surface failure; query an admitted operation's receipt before any explicit retry |

An undecodable header may not provide a trustworthy transfer ID. The endpoint
must reject it through the transport without inventing a correlated error frame.
Error 7 is available only inside an authenticated connection; it does not make
an unauthenticated endpoint or saved-state disclosure permissible.

## Transfer owner

[`esp_openclaw_setup_transfer.h`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_transfer.h)
implements these rules on the canonical serialized setup owner:

- Admit one logical transfer per current authenticated peer. Require increasing
  nonzero transfer IDs within that connection. Exhaustion requires a new
  authenticated session and local confirmation; never wrap to reuse an ID.
- Receive contiguous fragments from offset zero. A repeated fragment is
  acknowledged only when its total and bytes match a fully received range.
  Different fragment boundaries are allowed within that range. Gaps are invalid;
  changed totals, differing bytes, and overlaps extending past the received
  prefix are conflicts. A different peer cannot affect the current transfer.
- Hand off a complete request once, after checking the current generation/peer.
  The application must decode and admit operations separately before dispatch.
  A final acknowledgment means bytes were accepted, not that Wi-Fi or Gateway
  setup succeeded.
- Retain the completed transfer and response until the next transfer is admitted
  or setup closes. A repeated final fragment cannot dispatch twice. Keep enough
  request identity to check retransmissions after response generation; never
  overwrite the request buffer and then treat a matching ID alone as a duplicate.
- Return pending while the serialized owner prepares a response. Pulls are
  read-only and repeatable at a requested offset. They cannot trigger a mutation.
  A response beyond 8192 bytes fails with `TOO_LARGE`; an empty/null response
  fails with `INTERNAL`. Failure remains available to pull, and cannot be replaced
  by a late completion. Pulls at or beyond the response end are invalid.
- Keep mutating-operation receipts in the setup owner. A new transport transfer
  cannot bypass the owner's duplicate-operation protection. After a lost response,
  query that receipt rather than silently submitting a new operation ID.
- Bound buffers before allocation and clear credential-bearing storage when a
  retained transfer is retired or setup closes. Stop admitting work immediately
  on live transport revocation;
  do not wait for a queued UI disconnect notification.

The owner reserves separate fixed 8 KiB request and response buffers; keep its
storage outside small task stacks. Initialize once per boot and open once per
canonical setup generation. Closing wipes both entire buffers and does not
permit reopening the same generation to reset IDs. A higher ID can start only
at offset zero after the current response or failure is ready. An invalid start
does not consume an ID or discard the retained response. IDs never wrap.

Every receive, completion and pull checks the live canonical session. Poll even
without incoming frames. Revocation closes and wipes the transfer; the session
still owns cancellation and draining of admitted native operations. Completion
requires the exact pending generation, peer and transfer ID. A stranger's frame
or a stale completion cannot overwrite current work. Request views borrow
storage until the next admitted transfer or close; asynchronous consumers must
copy them. Input, output and owner storage must not overlap.

Replies require a buffer of at least 464 bytes. Rejected malformed or unauthorized
input leaves output bytes and arguments unchanged. The owner checks authority
again immediately before exposing a reply and request view; an endpoint that
awaits before sending must revalidate afterward. Transport exchange is allowed
in the comparison phase so the phone can show the challenge, but application
mutations and saved-state reads still require local confirmation.

## BLE integration requirements

The SDK's Security 2 implementation replaces its old session in
`new_transport_session`. The adapter must reject a second connection before
forwarding that call while another peer owns setup. A BLE connected event alone
does not prove successful authentication; a reusable connection handle is not a
boot-unique authenticated peer identity. Delegate cryptography to the SDK and
track ownership at its synchronous session boundary.

## Codec and fixtures

[`esp_openclaw_setup_frame.h`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_frame.h)
exposes allocation-free encode/decode. Decode returns a borrowed payload view;
keep the input alive until its consumer finishes or copies it. Encode requires
nonoverlapping input/output. Failure leaves output arguments unchanged.
Neither function authenticates, assembles transfers, parses application messages,
logs payloads, or calls a Wi-Fi/node owner.

For transfer `0x01020304`, a three-byte request `abc` is:

```text
4f 43 01 01 01 02 03 04 00 03 00 00 00 03 00 00 61 62 63
```

The [native tests](../../components/esp-openclaw-node-provisioning/tests/test_setup_frame.c)
include independent byte vectors for every kind, size/offset boundaries,
malformed input, and unchanged outputs after rejection. Firmware CI builds the
same codec; this is not evidence of an encrypted phone-to-device transfer.

The [transfer tests](../../components/esp-openclaw-node-provisioning/tests/test_setup_transfer.c)
exercise 8 KiB assembly, retry identity after response generation, repeatable
pulls, monotonic IDs, terminal errors, full-buffer wiping, stale completions,
live revocation, and the canonical confirmation/cancellation boundary. These are
native owner tests, not physical BLE or mobile SDK qualification.
