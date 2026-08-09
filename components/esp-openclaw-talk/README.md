# esp-openclaw-talk

`esp-openclaw-talk` adapts OpenClaw's Gateway-owned Talk API to Espressif's `esp_webrtc` signaling interface. Firmware requests `gateway-control-v1` with `talk.client.create`, then posts its local SDP to the returned Gateway offer URL with the returned single-use broker token.

The response must contain the exact descriptor `clientControl: { owner: "gateway" }`. Missing or different descriptors fail before peer creation, with no fallback to client-owned tool handling. Provider credentials and the agent-consult sideband stay on the Gateway.

Configure `esp_openclaw_talk_signaling_config_t` as `esp_webrtc_cfg_t.signaling_cfg.extra_cfg`, and use `esp_openclaw_talk_signaling_impl()` as the signaling implementation. The referenced `operator_node` must already be connected with `operator.talk`. `gateway_http_base_url` is required only for relative offer URLs.

`silence_duration_ms` optionally overrides the provider server-VAD post-speech
silence for that session. Leave it at zero to omit `silenceDurationMs` from
`talk.client.create` and preserve the Gateway configuration or provider
default.

The ESP WebRTC source is pinned as the repository submodule at `third_party/esp-webrtc-solution`; clone this repository with submodules enabled.
