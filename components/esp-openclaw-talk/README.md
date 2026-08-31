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
