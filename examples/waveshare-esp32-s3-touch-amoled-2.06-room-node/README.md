# Waveshare ESP32-S3 Touch AMOLED 2.06 room node

This example turns the watch-shaped Waveshare development board into a USB-powered, always-on OpenClaw room node. The straps are irrelevant; the firmware runs continuously with the AMOLED fully dark while idle.

It uses the maintained Waveshare BSP, the board's ES7210 microphone codec and ES8311 speaker codec, a 24 kHz stereo I2S path, Espressif's AEC capture source, and Espressif WebRTC. Ambient **Hi ESP** wake works through the WakeNet 9 (`wn9_hiesp`) instance already owned by the AFE; the firmware never creates the second esp-sr WakeNet instance that aborts inside the closed library on this hardware. Because esp_capture 1.0.2 does not expose AFE wake events, this example vendors its AEC source compilation unit and private-header closure in `main/`, adding the wake callback locally. That callback should move upstream and the vendored files should be dropped once esp_capture exposes detections. Gateway `voicewake.changed` events are observed, but an arbitrary text trigger cannot replace a compiled local WakeNet model.

## Build and provision

Build and flash with ESP-IDF release 5.5:

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

Provision over the USB Serial/JTAG console (the same cable that powers the board). The firmware boots into a `Setup` screen when it has no saved gateway session and starts the shared `openclaw>` REPL:

```text
openclaw> wifi set <ssid> <passphrase>
openclaw> gateway setup-code <code from `openclaw qr --voice-node --setup-code-only`>
openclaw> status
```

Wi-Fi credentials and the paired gateway session persist in NVS, so re-pairing to a different gateway later is one `gateway setup-code` away — no rebuild. The console `wake` command remains available to simulate the wake phrase for a Talk smoke test without speaking.

Kconfig provisioning still works for fleet builds: `idf.py menuconfig` under **OpenClaw room node** can bake Wi-Fi credentials and a setup code; baked Wi-Fi credentials seed NVS only while no console-set credentials exist. Leave provider/model/voice empty to use the Gateway Talk configuration, or set a model such as `gpt-live-1-codex` when the Gateway's OpenAI provider has access. GPT-Live also needs the Gateway HTTPS origin that corresponds to the setup code's `wss://` endpoint; this field is intentionally empty by default because its one-use Talk credential and SDP must not silently cross the LAN over plaintext HTTP.

Boot is deliberately resilient: Wi-Fi that cannot connect, a failed microphone path, or a missing setup code each log an error and leave the rest of the node running (Canvas works without the microphone; Talk wake stays off until audio init succeeds).

The first setup-code handshake stores two role-keyed device tokens against one hardware identity. The node-role connection advertises `talk`, `voiceWake`, and `canvas`; a separate operator-role connection requests only `operator.read` and `operator.talk`. Later boots reconnect both roles from NVS without retaining the one-time setup code in active session state.

## Runtime behavior

WakeNet remains active while playback is running and consumes the AEC-cleaned capture stream. A wake starts one client-owned WebRTC Talk session; the media peer connects directly to the configured provider while OpenClaw owns provider credentials and agent delegation. Calls close after the configured maximum lifetime, and another wake can start a new call.

Canvas mode uses a separate 410x502 LVGL screen at 80% brightness. Hiding it returns to the status screen; a user-initiated exit leaves a readable hint rather than dropping straight to the AMOLED-off idle policy. Talk state changes do not replace active Canvas content; a small status pill appears at the top-center until Talk returns to idle.

**Safe area.** The glass has large rounded corners, so laid-out content (A2UI, placeholder, test card) is inset 32 px — the same idea as Apple's safe area. Presented images are intentionally full-bleed, like wallpaper.

**On-device controls.** Tapping the screen or pressing the BOOT button cycles status, a safe-area test card, and a solid fill. The test card draws a border exactly on the safe-area inset with markers at its corners; the solid fill has no content variation, so together they separate panel/transfer artifacts from rendering artifacts without a serial console. The PWR button is hardware power management and is not GPIO-visible.

**Display buffers.** LVGL renders into two internal DMA-capable chunk buffers owned by this example rather than the BSP default. The BSP buffer comes from the default heap and lands in PSRAM, which this QSPI panel cannot DMA from; esp_lcd then bounce-buffers every flush through a fresh internal allocation that fails once Wi-Fi/TLS and audio claim internal RAM, silently dropping flushes. Two buffers also keep the renderer off memory the panel is still transmitting.

## Canvas commands

The node registers these seven commands. `placement` is accepted by `canvas.present` for protocol compatibility and ignored because this node always presents images fullscreen.

| Command | Parameters | Success payload |
| --- | --- | --- |
| `canvas.present` | `{"url":"https://host/image.png","placement":{"x":0,"y":0,"width":410,"height":502}}`; omit `url` to show the current A2UI surface or `Canvas ready` | `{"shown":true,"kind":"image","width":410,"height":502}` or `{"shown":true,"kind":"a2ui","components":3}` |
| `canvas.navigate` | `{"url":"https://host/image.jpg"}` | Same image payload as `canvas.present` |
| `canvas.hide` | `{}` | `{"hidden":true}` |
| `canvas.snapshot` | `{"format":"png","maxWidth":205,"quality":0.8}` | `{"format":"jpeg","base64":"<bytes>","width":205,"height":251}` |
| `canvas.a2ui.pushJSONL` | `{"jsonl":"{...}\n{...}"}` | `{"shown":true,"kind":"a2ui","components":3}` |
| `canvas.a2ui.push` | `{"messages":[{...},{...}]}` or `{"jsonl":"..."}`; `messages` wins when both are present | A2UI payload above |
| `canvas.a2ui.reset` | `{}` | `{"reset":true}` |

`canvas.present`, `canvas.navigate`, and A2UI `Image` components accept HTTP(S) URLs. A URL beginning with `/` is resolved through the Gateway's authenticated `canvas` plugin surface URL and the connected Gateway HTTP origin. The device follows redirects, uses the ESP certificate bundle for HTTPS, and accepts only content whose bytes begin with PNG or JPEG magic. Each fetch has a 10-second timeout.

Prefer JPEG for photos: the hardware-tuned decoder finishes a full-screen image in ~50 ms and decodes straight into the display buffer, while PNG goes through pure-C lodepng at roughly a tenth of that speed (~500 ms for the same frame). PNG remains the right choice for sharp-edged synthetic content.

Snapshots always return JPEG because the firmware carries one encoder; a requested `png` format is accepted but the payload truthfully reports `"format":"jpeg"`. `maxWidth` applies nearest-neighbor downscaling, and `quality` maps from 0..1 to JPEG quality 1..100 with a default of 0.80.

HTML responses fail with: `this canvas renders images and A2UI only; render HTML to a PNG/JPEG (e.g. browser screenshot) and present that URL, or use canvas.a2ui.pushJSONL`

SVG responses fail with: `SVG is not supported on this canvas; rasterize to PNG/JPEG first, or use canvas.a2ui.pushJSONL`

## A2UI v0.8 subset

Each JSONL line or `messages` entry must contain exactly one of these v0.8 actions. Messages containing `version` or `createSurface` are rejected.

| Action | Supported behavior |
| --- | --- |
| `surfaceUpdate` | Upserts `{"id":"...","component":{"Type":{...}},"weight":1}` entries into one global component store. The latest `surfaceId` is remembered. |
| `beginRendering` | Sets the root component, enters Canvas mode, and renders the surface. |
| `dataModelUpdate` | Replaces the model at an absent or `/` path, or sets a nested JSON-pointer path such as `/user/name`, creating intermediate objects. |
| `deleteSurface` | Clears the store like `canvas.a2ui.reset`. |

Component values may be `{"literalString":"..."}`, `{"literalNumber":1}`, `{"literalBoolean":true}`, or `{"path":"/user/name"}`. Missing data paths resolve to an empty string, zero, or false.

| Component | Rendering |
| --- | --- |
| `Column`, `List` | Vertical flex layout with ordered `children.explicitList`; start, center, end, and stretch cross-axis alignment are supported. |
| `Row` | Horizontal flex layout with the same child and alignment handling. |
| `Text` | Wrapped label with `h1`, `h2`, `h3`, `h4`, `h5`, `body`, and `caption` font fallbacks. |
| `Image` | Fetched PNG/JPEG centered and scaled to fit while preserving aspect ratio. |
| `Button` | LVGL button and label. A tap logs the action name locally; this build has no A2UI user-action back-channel. |
| `Card` | Padded, rounded dark container with a subtle border and one child. |
| `Divider` | One-pixel gray horizontal line. |
| `CheckBox` | Label plus non-interactive checked state resolved from `value`. |
| Unknown type | Caption label in the form `[TypeName]`. |

The surface is bounded to 96 stored components, 256 KiB per JSONL or messages input, 256 KiB of retained A2UI data, 8 component nesting levels, 6 images per surface, and 2 MiB per fetched image. Exceeding a bound returns `INVALID_PARAMS` and names the bound in the error message.

This pre-hardware build proves dependency resolution, model packaging, protocol code, and the complete ESP32-S3 image. Microphone geometry, speaker gain, false accepts, acoustic echo performance, thermals, and long-running room stability still require validation on the physical board.
