# esp-openclaw-talk

`esp-openclaw-talk` adapts OpenClaw's client-owned Talk API to Espressif's `esp_webrtc` signaling interface. Firmware asks the Gateway for a provider session with `talk.client.create`, then posts its local SDP to the returned offer URL with the returned short-lived credential.

The component is intentionally provider-neutral. OpenAI API keys and ChatGPT OAuth credentials stay on the Gateway. Standard OpenAI Realtime sessions return the direct `/v1/realtime/calls` offer URL. GPT-Live sessions return a Gateway-relative offer URL; the Gateway exchanges SDP with `/v1/live` and owns the agent-consult sideband.

Configure `esp_openclaw_talk_signaling_config_t` as `esp_webrtc_cfg_t.signaling_cfg.extra_cfg`, and use `esp_openclaw_talk_signaling_impl()` as the signaling implementation. The referenced `operator_node` must already be connected with `operator.talk`. `gateway_http_base_url` is required only for relative offer URLs.

`silence_duration_ms` optionally overrides the provider server-VAD post-speech
silence for that session. Leave it at zero to omit `silenceDurationMs` from
`talk.client.create` and preserve the Gateway configuration or provider
default.

The ESP WebRTC source is pinned as the repository submodule at `third_party/esp-webrtc-solution`; clone this repository with submodules enabled.
