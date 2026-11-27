# Shared device reader

This component owns the operator connection used to browse agents,
conversations, previews, and device inventory. It also owns reviewed replies,
conversation creation, and the existing messaging-access request flow. The
Dial uses this implementation; other boards can provide their own presentation
and inputs without copying its transport or connection-lifetime logic.

Call `esp_openclaw_reader_start(node, &config)` once per application after
creating the primary node. The configuration supplies the reader display name,
model, and optional expected TLS server name. Strings are copied during start;
omitting the configuration uses a generic reader identity. Pass the same TLS
server name as the primary node when using an encrypted passthrough relay.
`session_page_size` limits conversation rows per request. Zero preserves the
eight-row default; values from one to eight are accepted and copied at start.
Compact boards request two rows to leave room for response parsing and copying.
Pagination continues from the Gateway's returned `nextOffset`.

Set `compact_gateway_snapshots` to request the Gateway's `health-summary`,
`hello-health-on-demand`, `hello-presence-self`, and `chat-only-assistant-text`
capabilities on the operator connection. The reader does not use the full health
or startup presence snapshots;
these options reduce incoming messages on boards with limited contiguous heap.
Duplicate assistant-text events are omitted; canonical chat events remain
available.
The option defaults to false and is copied at start. Compact boards enable it.
The primary node's capabilities, commands, and approval remain unchanged. These
options are supported by Gateway 2026.11.6; older Gateways may send full snapshots.

Forward primary-node connection events to `esp_openclaw_reader_node_event`.
Keep the primary node alive for the reader's lifetime. The reader has no display
dependency and does not own the board's Wi-Fi or node command registration.

The separate connection starts with `operator.read`. It uses a saved operator
session for the same Gateway; node-only pairing does not supply read access.
It never grants itself access. Sending replies and creating conversations
remain explicit, revision-checked actions after messaging access is approved.
Neither reconnect nor a lost response automatically resends a message.
Failed reads emit `reader_response_diag` with numeric kind, stage, result and
byte count. Kinds are agents (0), sessions (1), preview (2), and creation (3).
Stages distinguish request allocation (1), a rejected or missing response (2),
response copy (3), parsing (4), request queueing (5), and the size limit (6).
For stage 2, result is the Gateway's success flag; allocation and queue stages
use `esp_err_t`, and parsing uses the directory result enum. These diagnostics
contain no payload, session key, agent identity, or credential. The SDK separately
reports `gateway_response_diag` when response serialization cannot allocate.

Applications can prepare their own text with `esp_openclaw_reader_reply_begin_text`.
It accepts at most 159 UTF-8 bytes, copies the draft, and binds it to the supplied
preview revision. Invalid or oversized text leaves the existing draft unchanged.
Preparation checks the selected conversation but sends nothing; the existing
reviewed `esp_openclaw_reader_reply_send` call remains the only submission step.
When a saved operator session exists, the reader tries to restore previously
granted read/write access. The Gateway checks this request against the saved
device token; it does not create an approval request. If authentication rejects
the requested scopes, the reader retries read-only once. A board with no saved
operator session still requires explicit read-only bootstrap setup.

For a node-only installation, `esp_openclaw_reader_setup_code(code)` queues one
bootstrap attempt without changing the primary node connection. The code must
contain a bootstrap token and the exact URI of the connected primary Gateway.
Shared-token/password codes, busy readers, and codes over 1,024 bytes are rejected.
The native connection owner checks the primary generation and a 60-second
deadline before transport, authentication, and credential persistence.

Pairing approval can follow an initial rejected attempt. Submit the code again
explicitly after approving the exact device's `operator.read` role request;
approval alone does not place the operator token on the board. The reader never
replays the code automatically. After success, discard it and reconnect with the
saved operator session. A limited bootstrap code can remain valid on the Gateway
until its expiry even when the board has discarded its copy.

Copy the agent, conversation, device, messaging, reply, and creation snapshots
into the UI owner. Submit actions with the revision from the displayed snapshot.
Replaced connections and selections invalidate stale snapshots and requests.
The public operations preserve the Dial's existing request limits and outcome
states, including unknown outcomes after an accepted request loses its response.

Conversation lists request the Gateway's `dashboard` row projection. It keeps
the identity, title, preview and pagination fields the reader consumes while
omitting unrelated runtime details that can exceed the device's receive limit.
The eight-row page and existing response-size checks remain unchanged. This
is validated with Gateway 2026.11.6. Older Gateways without this row mode
can reject the request; the reader does not retry with a larger projection. See
[session row modes](https://github.com/openclaw/openclaw/blob/f4f3651/packages/gateway-protocol/src/schema/sessions-list.ts).

The existing host suite exercises the shared implementation and the Dial's UI
contract:

```sh
python3 examples/m5stack-dial-node/tests/run_reader_host_tests.py \
  --cjson-dir /path/to/cJSON
```
