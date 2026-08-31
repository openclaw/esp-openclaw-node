# esp-openclaw-talk

`esp-openclaw-talk` adapts OpenClaw's Gateway-owned Talk API to Espressif's `esp_webrtc` signaling interface. Firmware requests `gateway-control-v1` with `talk.client.create`, then posts its local SDP to the returned Gateway offer URL with the returned single-use broker token.

The response must contain the exact descriptor `clientControl: { owner: "gateway" }`. Missing or different descriptors fail before peer creation, with no fallback to client-owned tool handling. Provider credentials and the agent-consult sideband stay on the Gateway.

Configure `esp_openclaw_talk_signaling_config_t` as `esp_webrtc_cfg_t.signaling_cfg.extra_cfg`, and use `esp_openclaw_talk_signaling_impl()` as the signaling implementation. The referenced `operator_node` must already be connected with `operator.talk`. Set `gateway_http_base_url` to the paired Gateway's HTTP(S) origin; negotiated offers use relative paths.

Leave `session_key` unset to use the Gateway's configured Talk agent. Before each
call, the adapter reads `talk.config` without secrets and combines `talk.agentId`
with `session.mainKey` (default `main`) into an agent-owned session key. This
lookup also requires `operator.read`; voice-node pairing grants both scopes.
The selected key stays fixed through call creation, cancellation, and close.
Automatic routing accepts valid ASCII agent IDs up to 64 bytes; routing inputs
and the completed session key must each fit within 256 bytes.

A nonempty `session_key` is used unchanged and skips configuration discovery.
When no Talk agent is configured, automatic routing keeps the existing `main`
request and lets the Gateway resolve its default owner or reject ambiguity. On
an explicit multi-agent Gateway, configure `talk.agentId` or supply an owned key
such as `agent:assistant:main`; the adapter never picks the first listed agent.
A failed configuration lookup does not fall back to another session. See
[Talk configuration](https://docs.openclaw.ai/nodes/talk).

`silence_duration_ms` optionally overrides the provider server-VAD post-speech
silence for that session. Leave it at zero to omit `silenceDurationMs` from
`talk.client.create` and preserve the Gateway configuration or provider
default.

The ESP WebRTC source is pinned as the repository submodule at `third_party/esp-webrtc-solution`; clone this repository with submodules enabled.

## Prepared-call lifetime

Owners that close WebRTC asynchronously should prepare a call before scheduling
start with `esp_openclaw_talk_call_prepare`. This copies the configuration and
routing inputs. Set its `closed` handler before start, then bind
`esp_openclaw_talk_call_signaling_impl()` with `extra_cfg = &call` and
`extra_size = sizeof(call)`. The SDK copies that pointer-sized reference; the
application keeps its owner reference until after SDK close. The original
`esp_openclaw_talk_signaling_impl()` remains available and shares the same
implementation. Neither the Talk nor Node configuration aggregate has changed
size, alignment, field order, or initializer contract.

Feed the source Node handle, event name, and borrowed JSON synchronously to
`esp_openclaw_talk_call_gateway_event`. Only `talk.event` containing exact
`session.closed`, the returned `voiceSessionId`, and an identical nested
`talkEvent.sessionId` closes the call. Identity is published before ICE or
connected notification. Ambiguous, malformed, unrelated and duplicate events
have no effect. `final: true`, recoverable `session.error`, and
`session.replaced` do not establish termination. The closed handler is a
nonblocking stop notification, separate from the existing setup-failure handler.
The room owner additionally fences event ingress by connection incarnation and
all deferred work by call generation.

On cancellation, call `esp_openclaw_talk_call_cancel` immediately. This seals
further SDK/application callback admission and prevents late config from
submitting create. From a worker, after start returns, call
`esp_openclaw_talk_call_quiesce`, then `esp_webrtc_close`, then
`esp_openclaw_talk_call_release`. Quiesce waits only for already-admitted
callback dispatches and nonblocking RPC submissions, without holding a lock.
It does not wait for remote RPC or HTTP completion. Keep SDK/application
contexts alive through SDK close; never close or quiesce from Node/peer/Talk
callbacks. The pinned SDK closes its peer before signaling stop, so sealing
only inside signaling stop is too late. Legacy signaling users must continue
to externally serialize their callback/context lifetime; use the prepared
binding for the pre-close barrier.

Pending RPC/HTTP references may outlive the application owner. A late create
success closes only its original returned voice ID with its frozen operator and
session key. Late failure and SDP completion cannot call sealed contexts.
The operator Node must outlive all queued RPC cleanup: reconnect using the same
Node. Node destruction does not promise to dispatch every queued RPC callback
and is not a cleanup/drain mechanism for a prepared call. Retain a call under
the owner's lock before handing it to another event dispatcher, and release
that reference after synchronous ingress returns. There is one teardown worker
per owner; quiesce/SDK close/release must be serialized.
