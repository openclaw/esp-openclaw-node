# Agent, conversation, and device directories

A bounded model for device pickers over the Gateway's `agents.list` response.
It retains eight rows at a time: canonical ID, display name and identity emoji.
The identity name takes precedence over the configured name; a missing name
falls back to the ID. Remote avatar URLs and unrelated metadata are not retained.

The page also retains the Gateway's `mainKey` suffix, unchanged, up to 128 UTF-8
bytes. Missing, empty, mistyped, or longer keys remain unavailable; the roster
can still be browsed. Routing keys are never shortened or replaced with a
guessed `main` value. This metadata does not select an agent or start a call.
The suffix comes from the Gateway's
[agent-list projection](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/src/gateway/agent-list.ts#L81).

`esp_openclaw_directory_parse` accepts at most 32 KiB and 16 levels of JSON
nesting. Names and emoji have UTF-8-safe byte limits. IDs must already be
canonical lowercase identifiers of at most 64 bytes; they are never shortened.
Invalid UTF-8, embedded NULs, invalid IDs, duplicate IDs, malformed responses
and out-of-range page offsets
reject the page. An empty successful roster is distinct from a parse failure.

The current [Gateway contract](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/packages/gateway-protocol/src/schema/agents-models-skills.ts#L145)
accepts an empty parameter object and returns the whole roster. The offset is
local paging, not server-side pagination. Every refresh still receives and
parses the whole response, subject to the device's transport and memory limits.
Listing an agent does not establish its availability for a later operation;
the Gateway must authorize that operation separately.

The agent and conversation parsers own no network client, selection, credentials or persistence.
The caller must reject responses from replaced connections before exposing a
page. The Dial's host runner exercises the parser and that lifecycle together:

```sh
python3 examples/m5stack-dial-node/tests/run_reader_host_tests.py \
  --cjson-dir examples/m5stack-dial-node/managed_components/espressif__cjson/cJSON
```

## Local agent avatars

`esp_openclaw_agent_avatar` maps configured 🦊 and 🦞 identity emoji (with an
optional variation selector) to bundled 64 x 64 artwork. It never infers an
animal from a name or ID. A single printable ASCII identity stays text; other
identities use up to two ASCII word initials, then the first alphanumeric ID
character, then `?`. Watch and Dial use the same mapping for list and detail
views. Remote URLs are not fetched.

`esp_openclaw_avatar_pixels` returns immutable BGRA pixels for supported images
and `NULL` for text. The component has no LVGL dependency. Artwork provenance
and regeneration instructions are in [assets/NOTICE.md](assets/NOTICE.md).

## Conversation pages and previews

`esp_openclaw_sessions_parse` consumes a server page from `sessions.list`,
requested with an explicit `agentId`, `limit: 8`, and `includeGlobal: false`.
It preserves each returned key up to 512 bytes; an oversized key rejects the
page instead of shortening or reconstructing a routing identity. Every key
must belong to the requested agent. Duplicate keys, conflicting agent IDs,
mismatched offsets, and more than eight rows reject the page. Titles and the
last-message snippet have UTF-8-safe display limits.

Only an explicit `hasMore: true` with a forward `nextOffset` enables another
page. A response without pagination metadata is terminal. Empty pages remain
successful results. These fields follow the Gateway's
[session-list schema](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/packages/gateway-protocol/src/schema/sessions-list.ts)
and [pagination implementation](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/src/gateway/session-utils-list.ts#L376).
Global rows are excluded because their bare keys do not carry an agent identity.

`esp_openclaw_preview_parse` consumes a single-key `sessions.preview` response
requested with `limit: 4`. The response must match the requested key exactly.
It preserves user, assistant, tool, system, and other role labels, with at most
four messages and 240 UTF-8 bytes of display text per message. The Gateway's
`empty`, `missing`, and `error` outcomes remain distinct from an invalid payload.
A missing conversation must not display previously cached messages. The
[preview handler](https://github.com/openclaw/openclaw/blob/5a4472678cfc08d5d286bcebec8889f8cb414fb7/src/gateway/server-methods/sessions-read.ts#L595)
rechecks visibility before reading a transcript.

Both parsers share the existing 32 KiB JSON, UTF-8, embedded-NUL, and depth
bounds. They clear output on failure and own no request lifecycle or UI state.
The caller must fence responses against the current connection and selection.
This model adds no conversation mutation or device-control assignment.

## Device inventory

`esp_openclaw_devices` owns a bounded, read-only `node.list` request lifecycle
for a caller-supplied operator client. The caller supplies its owner mutex and
wake callback, publishes client incarnation and connection changes while holding
that mutex, and calls `esp_openclaw_devices_poll` from one worker. Defer new
requests while higher-priority work is pending. This module creates no client,
requests no scopes, and performs no device commands.

Refresh expresses interest; cancel clears it and releases the retained page.
Responses are fenced by client, incarnation, connection, and view revision.
Only one transport request can remain outstanding; each interested view has a
15-second deadline even after a close/reopen invalidates an older request.
Snapshots distinguish idle, offline, loading, ready, empty, error, oversized,
and timed-out states. Copy them outside the caller's owner lock.

Failed device loads emit `devices_response_diag` with a numeric stage, result,
and byte count. Stages are request allocation (1), missing or rejected response
(2), response size limit (3), response copy (4), page allocation (5), parsing (6),
request queueing (7), and timeout (8). Parsing reports the directory result enum;
other stages report `esp_err_t`. A parse result of `OK` with a failed load means
the JSON was valid but did not satisfy the device-list schema. No payload,
device identity, address, or credential is logged.

Compact displays show the device load state in the list heading and keep the
Refresh action available. Local USB workspace status includes `deviceState`,
using the `esp_openclaw_devices_state_t` values above, from the same rendered
snapshot as `workspaceRevision`. This field is not added to remote commands.

The inventory retains at most 16 devices from a 32 KiB response. Each device
keeps its canonical ID, display name, model, and separate known flags for
connection and pairing. Missing optional facts remain unknown. Discovery
confers no control authority and changes no agent selection.

Device snapshots also retain exact `display.show` and `display.status`
advertisements from the Gateway's `commands` array. Missing, malformed,
duplicate, or oversized command fields remain unknown; an empty valid array
means neither command was advertised. Parsing uses the same UTF-8,
embedded-NUL, depth, and JSON bounds as the other directories.
These flags grant no permission and do not establish a command's parameter
schema. A control must still verify the target adapter, live connection,
and operator authority before submission. The field follows the
[Gateway node-list type](https://github.com/openclaw/openclaw/blob/f4f3651/src/shared/node-list-types.ts).

`esp_openclaw_display_receipt_parse` validates compact display responses to
`node.invoke`. It checks the node ID, command, heading, text, and numeric
generations before exposing a receipt. Acceptance and confirmed rendering
remain separate. Both structured `payload` and encoded `payloadJSON` are
supported; if both are present, they must agree. Malformed or mismatched
responses clear the output and cannot establish completion.

## Identify command lifecycle

`esp_openclaw_identify` prepares a short display code for a reviewed target.
The owner supplies the same mutex used by the device directory, publishes
connection and write readiness while holding it, and polls from one worker.
Begin and Send are separate calls. Send requires the current review revision,
an unchanged inventory revision, a connected and paired compact board, an
advertised `display.show` command, and an active writable operator connection.
Only the M5StickC and T-Embed display adapters are supported. The Gateway still
enforces its command allowlist and permissions.

A submission calls `node.invoke` once with a unique idempotency key. It does
not bind an agent or conversation. The response must match the target and
display code. Accepted and rendered are separate states. A lost connection,
deadline, or invalid receipt after submission produces an unknown outcome;
the module never retries. The callback retains its slot until completion,
even after a timeout. A failed local queue admission means nothing was sent.

This component supplies the lifecycle API; applications must provide the
review and result screens before exposing the action.
