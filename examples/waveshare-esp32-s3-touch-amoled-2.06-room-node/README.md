# Waveshare ESP32-S3 Touch AMOLED 2.06 room node

This example turns the watch-shaped Waveshare development board into a USB-powered, always-on OpenClaw room node. The straps are irrelevant; the firmware runs continuously with the AMOLED fully dark while idle.

It uses the maintained Waveshare BSP, the board's ES7210 microphone codec and ES8311 speaker codec, a 24 kHz stereo I2S path, Espressif's AEC capture source, WakeNet 9, and Espressif WebRTC. The compiled wake phrase is **Hi ESP** (`wn9_hiesp`). Gateway `voicewake.changed` events are observed, but an arbitrary text trigger cannot replace a compiled local WakeNet model. A custom “OpenClaw” wake phrase requires a matching Espressif model artifact.

## Provision and build

1. On the Gateway, run `openclaw qr --voice-node --setup-code-only`.
2. Run `idf.py menuconfig` and set Wi-Fi and the setup code under **OpenClaw room node**.
3. Leave provider/model/voice empty to use the Gateway Talk configuration, or set a model such as `gpt-live-1-codex` when the Gateway's OpenAI provider has access. GPT-Live also needs the Gateway HTTPS origin that corresponds to the setup code's `wss://` endpoint; this field is intentionally empty by default because its one-use Talk credential and SDP must not silently cross the LAN over plaintext HTTP.
4. Build and flash with ESP-IDF release 5.5:

   ```sh
   idf.py set-target esp32s3
   idf.py build flash monitor
   ```

The first setup-code handshake stores two role-keyed device tokens against one hardware identity. The node-role connection advertises `talk`, `voiceWake`, and `display`; a separate operator-role connection requests only `operator.read` and `operator.talk`. Later boots reconnect both roles from NVS without retaining the one-time setup code in active session state.

## Runtime behavior

WakeNet remains active while playback is running and consumes the AEC-cleaned capture stream. A wake starts one client-owned WebRTC Talk session; the media peer connects directly to the configured provider while OpenClaw owns provider credentials and agent delegation. Calls close after the configured maximum lifetime, and another wake can start a new call.

This pre-hardware build proves dependency resolution, model packaging, protocol code, and the complete ESP32-S3 image. Microphone geometry, speaker gain, false accepts, acoustic echo performance, thermals, and long-running room stability still require validation on the physical board.
