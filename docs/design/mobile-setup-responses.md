# Mobile setup responses

Watch and Dial share this version 1 response format inside a completed
[setup transfer](mobile-setup-frames.md). The encoder is implemented; dispatch,
BLE and phone integration remain separate work. A response is at most 596 bytes
and still uses bounded transport fragments, never one oversized attribute value.

## Header and result codes

Integers are unsigned and big-endian, except the signed RSSI byte. The header
is exactly 16 bytes. Bodies have no optional fields; consume their exact length
and reject unknown values, nonzero reserved bytes, and trailing bytes.

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 1 | Application version: `01` |
| 1 | 1 | Original [request operation](mobile-setup-requests.md), or 0 for a decoding error |
| 2 | 2 | Public result code below |
| 4 | 8 | Generation; current authenticated generation for `hello`, otherwise original request generation |
| 12 | 4 | Original operation ID; 0 for `hello`, scans and current status |

Decoding errors use operation, generation and operation ID zero: malformed
input has no trusted identity to echo. Preserve all 64 generation bits.

| Code (hex) | Meaning |
| --- | --- |
| `0000` | Read succeeded: `hello`, `status`, or `wifi.scan` body follows |
| `0001` | Mutation accepted; receipt body follows, not a promise of connection or storage |
| `0002` | Duplicate mutation ID; original receipt body follows, no redispatch |
| `0100` | Invalid request or conflicting operation kind |
| `0101` | Unsupported application version |
| `0102` | Unknown operation |
| `0103` | Request too large |
| `0104` | New local physical confirmation required |
| `0105` | Operation or native owner busy |
| `0106` | Receipt limit reached; close and reopen setup with fresh confirmation |
| `0107` | Operation receipt not found |
| `0108` | Native operation unsupported |
| `0109` | Native owner unavailable |
| `010a` | Internal failure |
| `010b` | Stale request generation |

Every `01xx` error has an empty body. Codes are explicit wire constants; never
serialize local codec/session enum ordinals, raw `esp_err_t`, pointers or structs.
Cancel has no success response: closing revokes reply authority. The phone must
tolerate disconnection and must not infer rollback from it.

## Hello body

One phase byte (1 comparison, 2 ready, 3 busy), one label-length byte and 1–64
original label bytes, one model-length byte and 1–64 original model bytes, then
exactly 32 challenge bytes. Label/model contain no NUL; they carry no network or
account information. Their bytes are not trimmed or truncated by the encoder.

The challenge belongs to the same encrypted peer/comparison owner used by
`setup_attach` and the local display. The codec only borrows those bytes for the
encoding call; it never generates, stores or replaces a challenge. The session
owns generation and phase. A hello cannot confirm a peer or expose saved state.

## Status and mutation receipt body

Current `status` (request ID 0) describes the latest canonical receipt, if any.
Nonzero-ID status returns that exact receipt. Apply replies use the same body.

| Body offset | Width | Field |
| --- | --- | --- |
| 0 | 1 | Current session phase: 2 ready or 3 busy |
| 1 | 1 | Canonical receipt count: 0–32 |
| 2 | 1 | Receipt operation: 0 none, 4 Wi-Fi apply, 5 Gateway apply |
| 3 | 1 | Outcome: 0 none, 1 pending, 2 succeeded, 3 failed, 4 unknown after drain |
| 4 | 4 | Receipt operation ID, or 0 without a receipt |
| 8 | 2 | Receipt detail category below |
| 10 | 1 | Effects known mask: bit 0 save observation available; all other bits zero |
| 11 | 1 | Effects value mask: bit 0 save confirmed by that attempt; unknown bits zero |
| 12 | 1 | Native progress: 0 unknown, 1 preparing/queued, 2 connecting, 3 draining, 4 recovery required, 5 terminal |
| 13 | 3 | Reserved: all zero |

Detail categories: 0 none, 1 invalid input, 2 unsupported, 3 busy, 4 timeout,
5 cancelled, 6 resource failure, 7 operation failed. Only failed receipts carry
a nonzero category. Map the receipt's own stored detail, never the newest
attempt's error or disconnect reason. A status without a receipt has operation,
ID, detail, effects and progress zero, count zero and ready phase; it is only
valid for current status.

Effects and native progress are observations from an **exact generation and
operation-ID match** with the binding's native attempt. Older receipts whose
attempt was replaced keep their own outcome/category but have unknown effects
and progress. Do not expand receipts or create another effects cache. A known
false save observation means no save has been confirmed by that attempt; it
does not mean credentials are absent, old credentials survived, or anything was
rolled back. Unknown observations must stay unknown in phone progress and errors.

Complete an apply transfer immediately with its accepted/duplicate receipt so
it does not block newer transfers. Fetch terminal outcomes with a **new status
transfer**, retaining the same operation ID. Lost acknowledgments never justify
a fresh mutation ID or automatic replay. A stable response retained by the
transfer is a snapshot, not a live operation view.

## Scan body

One count byte (0–16), one truncation byte (0 or 1), then two bytes for the total
native result count. Truncation is exactly `total > count`. Each record has one
SSID-length byte (0–32), those original SSID bytes, one signed two's-complement
RSSI byte, one channel byte, and one security byte: 0 open, 1 password, 2
unsupported. SSID bytes need not be UTF-8 and are never normalized. A zero-length
SSID cannot be submitted; offer manual entry. The current native scan omits
hidden SSIDs. Unsupported security requires another setup method.

Scanning requires confirmed, idle READY state and the existing Wi-Fi lease.
It must preserve the current link and refuse to scan while connected or
reconnecting; offer manual SSID entry or Other ways instead of disconnecting.

## Encoder and independent vectors

[`esp_openclaw_setup_response_encode`](../../components/esp-openclaw-node-provisioning/include/esp_openclaw_setup_response.h)
allocates nothing and validates the complete response before writing any byte.
Input, borrowed spans and output must not overlap. Invalid shapes or insufficient
capacity leave both output bytes and output length unchanged.

The [native tests](../../components/esp-openclaw-node-provisioning/tests/test_setup_response.c)
compare independent hello, pending, older failed, duplicate succeeded, empty
status, scan and error vectors. They cover exact maximum bodies, one-byte-short
capacities, unknown values and inconsistent knowledge/identity fields under
ASan/UBSan. This proves encoding, not authentication, dispatch or BLE pairing.
