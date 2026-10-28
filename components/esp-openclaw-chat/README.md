# ESP OpenClaw chat client

A bounded, single-message client for an existing conversation, shared by device
UIs. It uses the Node's authenticated Gateway connection. This component does
not add a screen, change device permissions, record audio, create conversations,
or speak assistant replies.

## Flow

1. Call `esp_openclaw_chat_prepare(node, selected_key, selected_agent, &chat)`.
   It reads one history entry with a bounded content budget. Wait for `READY`.
   Preparation requires an existing session ID, an authoritative canonical key,
   the selected agent, and an explicit active-leaf value (including `null`).
2. Show the completed draft for review. Only an explicit user action should call
   `esp_openclaw_chat_send(chat, text, unique_intent_id)`.
3. Forward the shared Node event callback to `esp_openclaw_chat_event`. Poll and
   copy state from the UI owner. `SENDING` means local queue acceptance;
   `RUNNING` means the Gateway acknowledged the run or emitted matching activity.
4. Use the existing conversation reader to retrieve replies. `COMPLETE` describes
   this run, including a queued followup's admission run; it does not promise
   that all agent work has finished. `HANDED_OFF` means the final event yielded
   work elsewhere. Neither state invents reply text.
5. Destroy the handle when leaving its view. Destruction never aborts the turn.

Each handle submits at most one message. `chat.send` captures the prepared
`sessionKey`, `agentId`, `sessionId` and `expectedLeafEntryId`, uses the supplied
`idempotencyKey`, `queueMode: "followup"`, and `deliver: false`. The server validates
session and branch changes. The Node must already hold `operator.read` and, to
send, `operator.write`; this component requests no additional scopes.

A rejected local queue operation leaves `READY`, so the user can retry that
unsent draft. After queue acceptance, a missing/malformed reply, lost connection,
or timeout becomes `OUTCOME_UNKNOWN` rather than a safe-to-resend failure. Some
Gateway errors cannot establish whether the original turn ran; the client keeps
that uncertainty. There is no automatic replay or reconnect retry. Read the
conversation to reconcile it; do not silently submit a fresh intent ID. Matching
late events on the original connection can still resolve an unknown outcome.

The original Node must outlive the handle **and all accepted RPC callbacks**.
Callbacks retain the handle until they return. Callers serialize handle
publication, event forwarding, polling, snapshots, Send, and destruction; the
client synchronizes asynchronous transport callbacks internally. Never access a
handle after destruction. A prepared handle is tied to its original connection;
reconnect requires fresh preparation, not reuse of old conversation state.

## Bounds and validation

- Draft: 2,047 UTF-8 bytes; whitespace-only drafts are rejected.
- Canonical key: 512 bytes; agent: 64 bytes; session, leaf and intent IDs: 128 bytes.
- Incoming JSON: 16 KiB and 16 nesting levels. Routing strings are never truncated.
  Duplicate properties, escaped NUL and invalid UTF-8 are rejected.
- RPC response timeout: 30 seconds; run inactivity timeout: five minutes.
- Run events match connection, run ID, canonical session key and, when supplied,
  agent ID. Sequence zero is valid; duplicate and older events are ignored.

This is client/transport boundary proof, not hardware or live-Gateway proof:

```sh
python3 components/esp-openclaw-chat/tests/run_host_tests.py --sanitize \
  --idf-path /path/to/esp-idf --cjson-dir /path/to/cJSON
```

The same Unity cases compile into the Node firmware test app. The host runner
reuses the existing Talk suite's ESP/FreeRTOS header shims, with a separate chat
runner and no audio/network stubs. Its pthread mutex shim verifies ownership
ordering, not FreeRTOS scheduling or timing. CI builds the real component and
runs the host suite. Watch and Dial UI adoption are separate changes.
