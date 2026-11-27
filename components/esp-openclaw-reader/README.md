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

Forward primary-node connection events to `esp_openclaw_reader_node_event`.
Keep the primary node alive for the reader's lifetime. The reader has no display
dependency and does not own the board's Wi-Fi or node command registration.

The separate connection starts with `operator.read`. It uses a saved operator
session for the same Gateway; node-only pairing does not supply read access.
It never grants itself access. Sending replies and creating conversations
remain explicit, revision-checked actions after messaging access is approved.
Neither reconnect nor a lost response automatically resends a message.

Copy the agent, conversation, device, messaging, reply, and creation snapshots
into the UI owner. Submit actions with the revision from the displayed snapshot.
Replaced connections and selections invalidate stale snapshots and requests.
The public operations preserve the Dial's existing request limits and outcome
states, including unknown outcomes after an accepted request loses its response.

The existing host suite exercises the shared implementation and the Dial's UI
contract:

```sh
python3 examples/m5stack-dial-node/tests/run_reader_host_tests.py \
  --cjson-dir /path/to/cJSON
```
