# Agent and conversation directory model

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

This component owns no network client, selection, credentials or persistence.
The caller must reject responses from replaced connections before exposing a
page. The Dial's host runner exercises the parser and that lifecycle together:

```sh
python3 examples/m5stack-dial-node/tests/run_reader_host_tests.py \
  --cjson-dir examples/m5stack-dial-node/managed_components/espressif__cjson/cJSON
```

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
