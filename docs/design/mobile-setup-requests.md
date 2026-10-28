# Mobile setup requests

Watch and Dial share this version 1 application request format inside a completed
[setup transfer](mobile-setup-frames.md). The decoder is implemented; response
messages, dispatch, BLE and mobile integration remain separate work. Decoding
does not authenticate a peer, confirm a device, or apply settings.

## Header

All integers are unsigned and big-endian. The header is exactly 16 bytes, followed
by the command's exact body. Reserved bytes must be zero. Reject unknown versions
and operations, missing fields, extra bytes and requests over 8192 bytes before
allocation or mutation. There are no optional fields or generic key/value maps.

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 1 | Application version: `01` |
| 1 | 1 | Operation, below |
| 2 | 2 | Reserved: `00 00` |
| 4 | 8 | Expected setup generation |
| 12 | 4 | Operation ID |
| 16 | variable | Command body |

Preserve all 64 generation bits. A mobile implementation must use an exact
integer representation, not a floating-point number. The authenticated peer
identity comes from the transport context, never from these bytes. A structurally
valid generation does not prove it is current; the serialized setup owner must
check live authority again before exposing saved state or applying an effect.

## Operations and bodies

| Operation | Value | Generation | Operation ID | Body |
| --- | --- | --- | --- | --- |
| `hello` | `01` | 0 | 0 | Empty |
| `status` | `02` | Nonzero | 0 for current status, nonzero for an operation receipt | Empty |
| `wifi.scan` | `03` | Nonzero | 0 | Empty |
| `wifi.apply` | `04` | Nonzero | Nonzero | SSID field, then password field |
| `gateway.apply` | `05` | Nonzero | Nonzero | Setup-code field |
| `cancel` | `06` | Nonzero | 0 | Empty |

`hello` bootstraps the generation after encrypted authentication because the QR
does not contain it. It may return only the device label/model, supported version,
current generation and comparison challenge. It never exposes saved network or
account data. `status`, scans and mutations require local device confirmation in
addition to current generation and peer ownership. No request can confirm itself.

Each field starts with a two-byte length, then exactly that many original bytes.

| Field | Byte length | Meaning |
| --- | --- | --- |
| SSID | 1–32 | Original SSID supported by the native Wi-Fi owner |
| Password | 0–64 | Original passphrase or 64-byte PSK; empty for an open network |
| Setup code | 1–4096 | Original code passed to the native setup-code parser |

No field may contain NUL: the existing Wi-Fi and setup-code APIs take C strings.
Otherwise the decoder preserves bytes, including whitespace and non-UTF-8 SSIDs;
it does not trim, normalize, decode Base64URL or validate network credentials.
The 4096-byte limit applies to the code in this field, before the native parser's
Base64URL decoding, not the inner JSON size. Credential and destination validity
remain the native owner's responsibility. Successful decoding does not replace
the required destination display and confirmation.

`cancel` closes this peer's entire current setup session, including any pending
operation. It does not select an operation or roll back earlier changes. Allow
it during comparison as well as confirmed setup, but still require the current
authenticated peer and generation. Closing revokes transport authority, so the
phone must tolerate disconnection without an application reply. Native owners
continue draining and retain local observed effects; a new session requires fresh
authentication and confirmation. Do not infer an operation's final effect from
the disconnect or promise that its old receipt survives session closure.

## Retry and storage ownership

Transfer IDs correlate byte exchanges; operation IDs identify mutations within
one setup generation. They are independent. After a lost response, query `status`
with the original operation ID using a new transfer. Never silently generate a
new operation ID and repeat a write.

The setup owner retains up to 32 mutation receipts. Repeating an operation ID
with the same operation kind returns its original receipt without replacing its
payload or dispatching again. Another kind with that ID is invalid. The owner
does not compare payload bytes across different transfers: an altered payload
with the same operation ID cannot update the original operation. Byte-conflict
detection belongs to retries within the same transfer.

[`esp_openclaw_setup_request_decode`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_request.h)
allocates nothing and returns borrowed spans. Keep input alive until its consumer
finishes or copies it. Spans are not NUL-terminated; use bounded owned buffers
with room for the terminator when adapting to native C-string APIs. Copy before
queueing asynchronous work or retiring the transfer, and wipe credential copies
after their owner is done. Do not log payloads. Input and output must not overlap.
All failures leave the output unchanged.

Decoder results are local C API outcomes, not application response codes. Do not
serialize those enum ordinals, pointers or native structs. Define and test the
response/error schema before connecting a BLE endpoint.

## Independent wire examples

`hello`:

```text
01 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

`wifi.apply` for generation `0x0102030405060708`, operation `0x89abcdef`, SSID
bytes `20 41 ff 20`, and password bytes `20 70 20`:

```text
01 04 00 00 01 02 03 04 05 06 07 08 89 ab cd ef
00 04 20 41 ff 20 00 03 20 70 20
```

These are synthetic codec fixtures, not credentials or evidence of pairing.
The [native tests](../../components/esp-openclaw-node-provisioning/tests/test_setup_request.c)
cover every command, full-width identities, borrowed original bytes, empty and
maximum fields, unaligned input, truncated lengths/bodies, NULs and unchanged
output on failure. Firmware CI builds the same decoder for the supported boards.
