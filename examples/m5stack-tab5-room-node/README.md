# M5Stack Tab5 Room Node

Thin ESP32-P4 board adapter for the canonical `esp-openclaw-room-node`
component, targeting ESP-IDF 5.5.5 and the `espressif/m5stack_tab5` 1.2.0~1
source/API contract.

## Build and provision

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

### Isolated SDK compatibility patch

Tab5 CI applies `patches/esp-idf/tab5-spm-stack-sanity.patch` to SDK commit
`362a1776ec212788fda95f75b733bfdde3a0c394` before configuring the build. This is
ESP-IDF plus a tracked compatibility patch, not pristine ESP-IDF 5.5.5.

On ESP32-P4 v1.3, internal SPM can hold an ordinary task stack but is omitted
from this SDK's flash-operation stack-sanity predicate. See
[ESP-IDF issue 19020](https://github.com/espressif/esp-idf/issues/19020).
The patch adds only the `SOC_MEM_SPM_SUPPORTED`-guarded `esp_ptr_in_spm(sp)`
case. It retains the assertion and existing DRAM/RTC checks; it does not make
PSRAM stacks safe for flash access or change allocation policy.

The connected qualification board is ESP32-P4 v1.3. The existing profile sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`; inspected image headers have minimum
revision 0 and maximum revision 199. This is not evidence that the same binary
supports revision 3 or newer silicon. Other P4 revisions remain physically
unqualified, and this patch does not change chip-revision selections.

For manual reproduction, use a dedicated SDK checkout at the exact commit,
never a shared SDK installation. From this example directory, explicitly
apply the patch before `idf.py reconfigure` or `idf.py build`:

```sh
python3 ../../scripts/idf_tab5_compat.py --idf-path "$IDF_PATH"
python3 ../../scripts/idf_tab5_compat.py --idf-path "$IDF_PATH" --verify-only
```

The helper rejects another SDK base, unrelated tracked/untracked changes,
changed patch bytes, or an unexpected complete source file. An already-applied
exact patch is verified without reapplying it. CMake never edits the SDK.
CI uses only its isolated SDK container and does not change shared checkouts.

Firmware packaging independently verifies the actual SDK source delta, not a
marker file. Its manifest records the dirty SDK state, base commit, patch
SHA256, and patched-source SHA256; other examples still reject dirty SDKs.
The separate ELF/map artifact copies that same firmware manifest unchanged.
Retire the patch, helper and packaging exception together only after an
upstream SDK fix is selected and qualified on affected hardware.

### Camera CSI compatibility

Tab5 CI additionally applies `patches/esp-video/tab5-csi-post-isp-format.patch`
to the published `esp_video` 2.4.1 component at revision
`67a555a517b7aa4753836432453659abfaca393a`. That component's IDF<6 mapping asks
CSI to convert the sensor's RAW8 format to RGB565. The exact SDK above has
backported the post-ISP CSI contract: on P4 revisions below 3, CSI input and
output must match. The captured revision-1.3 failure was at `VIDIOC_STREAMON`,
errno 3, which this video component maps from `ESP_ERR_NOT_SUPPORTED`.
See [esp-video-components issue 98](https://github.com/espressif/esp-video-components/issues/98)
for a related report; its different board/sensor is not Tab5 qualification.

The patch keeps ISP conversion RAW8 to RGB565, then passes RGB565 to RGB565
through CSI. This preserves 16-bit output and DMA sizing; it does not pass
RAW8 through while claiming RGB565 buffers. Raw bypass, unsupported-format
checks, SDK guards, allocation, sensor settings, and chip-revision selections
are unchanged.

Use only the isolated SDK and diagnostic profile described on this page.
After SDK patching and `idf.py reconfigure`, apply the camera patch alongside
the SDIO patches below, then build with normal Ninja:

```sh
python3 ../../scripts/tab5_camera_compat.py --idf-path "$IDF_PATH" \
  --project-path . --component-path managed_components/espressif__esp_video --build-path build
ninja -C build -j2
python3 ../../scripts/tab5_camera_compat.py --idf-path "$IDF_PATH" \
  --project-path . --component-path managed_components/espressif__esp_video --build-path build --verify-only
```

Application is gated by the exact SDK commit and CSI source hash, unchanged
early-P4 profile, registry lock/manifest, original whole-component/source hashes,
and tracked patch hash. Another SDK is refused without modifying it; no version
macro is spoofed. After Ninja, verification checks the configured mapper path,
current RISC-V object, and final patched component/source hashes. Component-manager
integrity markers are never rewritten; stop if reconfiguration rejects a modified
component rather than bypassing its checks.

Packaging requires `--camera-compat` in addition to this branch's SDK/SDIO flags.
Its additive `camera_compatibility_patch` record retains SDK and SDIO provenance
unchanged, and the separate symbols artifact receives the same manifest. Host
tests execute the complete upstream mapper, including original-failure/patched-pass,
raw bypass, sibling outputs and rejection cases; they do not execute CSI DMA or
capture a frame. A successful native build is not physical camera qualification.
Retire this patch and its explicit packaging mode together after selecting and
qualifying an upstream component that matches the actual SDK contract.

### NVS diagnostic build

This diagnostic branch additionally applies
`patches/esp-idf/tab5-nvs-first-failure.patch` in Tab5 CI. It does not repair
session loss or change storage, retry, initialization recovery, or allocation
policy. The SDK base and SPM patch above remain unchanged. Manual diagnostic
builds must explicitly add `--nvs-diagnostics` to both helper commands above;
packaging requires the same option. Verification permits exactly the two
approved dirty SDK paths, with separate `compatibility_patch` and
`diagnostic_patch` manifest records. The symbol artifact retains that same
manifest.

The following unprefixed ROM-output records contain fixed numeric fields only.
Capture them before filtering for normal I/W/E log prefixes:

| Record | Fields |
| --- | --- |
| `nvs_io_diag` | `op`, `err`; first captured failure per boot |
| `nvs_session_diag` | `role`, `stage`, `err`, `presence` |
| `nvs_connect_diag` | `role`, `cached`, `fresh_known`, `fresh`, `err` |
| `nvs_init_diag` | `err`; initial initialization failure before existing recovery |

`op`: 1 read_raw, 2 read, 3 write_raw, 4 write, 5 erase_range.
Read/write alignment errors retain their original return and perform no I/O,
but also claim the first-failure record. The other records follow the underlying
partition call's return. The internal-RAM atomic claim is lock-free; output
does not allocate or hold that claim as a lock. No success records, offsets,
lengths, keys, URLs, IDs, or credential values are emitted by this SDK probe.

`role`: 0 unknown, 1 node, 2 operator.
Session `stage`: 1 open, 2 version, 3 URI size, 4 URI allocation/value read,
5 token size, 6 token allocation/value read, 7 validation, 8 clear result.
Session `presence` is a classification: 0 unread/not classified, 1 missing,
2 empty, 3 unsupported version, 4 incomplete fields, 5 invalid URI.
The original error precedes normalization or clear; clear has its own result.
An `err=0` clear result is not proof of a physical write.

Connect booleans are 0/1. `cached` is sampled under the state lock before
replacement, or from the retained cache after a load error; `fresh_known=0`
means a failed load, not authoritative absence. These are observations, not
authorization or new recovery behavior. ROM output can interleave with other
console output, and lack of a record does not prove healthy NVS. The SDK probe
covers these five NVSPartition methods, not every flash operation.

Host tests execute the patched SDK methods and actual session loader against
I/O stubs, including original return values, no-I/O alignment failures,
concurrent first capture, clear failures and synthetic canary non-disclosure.
The fixture SDK file is the exact Apache-2.0 source at the pinned commit;
normal CI still builds the real SDK. Target Unity lifecycle tests are built,
not run by CI. Remove this temporary diagnostic patch and its explicit
packaging mode after the observed failure is identified and qualified.

### Transport-start allocation diagnostics

Tab5 installs one global failed-allocation callback before board initialization.
Two private, default-no-op transport hooks arm only the calling node/operator
task around `client_start`. ISR and unrelated-task failures are ignored. The
callback captures the first request size, capability mask, and SDK function
pointer in internal RAM without dereferencing that pointer, logging, heap queries,
allocation, or waiting. After disarm, the task maps the pinned SDK's static
function name to a bounded allocator enum; no pointer or name is logged.
No other failed-allocation callback may be registered in this diagnostic profile;
the SDK registration API replaces the callback and has no getter.

After the SDK returns, the task disarms capture before querying the matching
capability heaps and logging, before existing transport-owner cleanup:

```text
alloc_diag phase=after_sdk_return_before_owner_cleanup role=<u32> sdk_err=<i32> captured=<0|1> requested=<u32> caps=<u32> allocator=<u32> origin=0 heap_sampled=<0|1> free=<u32> largest=<u32>
```

`role` is 1 node or 2 operator. `allocator` is 0 unknown, 1 `heap_caps_malloc`,
2 `heap_caps_malloc_default`, 3 `heap_caps_calloc`, 4 `heap_caps_realloc`,
5 `heap_caps_realloc_default`, 6 `heap_caps_malloc_prefer`,
7 `heap_caps_calloc_prefer`, or 8 `heap_caps_realloc_prefer`. It identifies the
reporting allocator API, **not** its caller; `origin=0` explicitly means unknown.
`requested` is the SDK callback's byte-count argument, preserved without
reinterpretation. In this SDK, `heap_caps_calloc_prefer` reports its element-size
argument, so it must not be treated as independently verified total bytes.

The heap samples are **after SDK return**, not fault-time heap availability.
The SDK may already have freed resources. They do not prove alignment fit or a
task-stack allocation cause. A failed SDK start without a matching callback
still emits a record: `captured=0 heap_sampled=0`, and zero request/heap fields
mean unavailable, not an empty heap. A captured allocation followed by SDK
success is also reported; allocation capture alone does not mean start failed.
Uncaptured success is silent. Registration failure emits only
`alloc_diag_install err=<i32>` and does not change startup error handling.

Host fixtures exercise the actual callback/start boundary; Tab5 CI also verifies
that both final ELF hook definitions are strong. Other boards retain no-op hooks.
These diagnostics change no stack sizes, allocator policy, SDK/dependency
patches, retries, or cleanup. Physical allocation/reconnect proof remains a
separate qualification gate.

### Streaming RX PSRAM and allocation diagnostics

This branch also applies `patches/esp-hosted/tab5-sdio-first-allocation-failure.patch`
to the registry `esp_hosted` 1.4.0 component at revision
`6040085eefe908de68fcd3438bf10b8ecd6de0c6`. It observes the streaming RX buffer's
first failed allocation. A second patch,
`patches/esp-hosted/tab5-sdio-streaming-rx-psram.patch`, places only the two
growable streaming RX buffers in PSRAM when PSRAM and SDMMC PSRAM DMA support
are enabled. It uses `esp_dma_capable_malloc` with explicit SPIRAM/8-bit
capabilities and the existing 64-byte DMA alignment; the SDK also accounts
for cache alignment. Other targets retain the original DMA allocation.

This addresses a captured 6,144-byte streaming allocation failure with only
a 3,072-byte largest DMA block. TX, register buffers, packet pools, assertions,
512-byte padding, reads, counters, task stacks, SDK patches, and configuration
are unchanged. It is not a claim that all SDIO allocation failures are resolved;
this profile still needs physical qualification.

In an isolated configured project, explicitly apply the component patch
after `idf.py reconfigure` and before normal Ninja:

```sh
python3 ../../scripts/tab5_sdio_diagnostics.py --psram-rx \
  --project-path . --component-path managed_components/espressif__esp_hosted --build-path build
ninja -C build -j2
python3 ../../scripts/tab5_sdio_diagnostics.py --psram-rx \
  --project-path . --component-path managed_components/espressif__esp_hosted --build-path build --verify-only
```

The helper requires the pinned lock identity, manifest/revision, whole component
and source hashes, patch hash, and configured compile input. Verification checks
actual bytes and permits exactly the selected patch sequence, not arbitrary local edits.
Do not rewrite component-manager integrity metadata or bypass a reconfiguration
failure. Packaging requires `--sdio-psram-rx --sdio-diagnostics` alongside
`--nvs-diagnostics`. The `component_compatibility_patch` manifest record lists
both ordered patches, their intermediate source hashes, and the final source
and whole-component hashes. The symbol artifact retains that same manifest.
Omit `--psram-rx` and `--sdio-psram-rx` only for the original diagnostic-only
profile, whose manifest retains `component_diagnostic_patch`.

One unprefixed ROM line has this exact field order, with decimal integers and
no trailing period:

```text
sdio_alloc_diag requested=%u padded=%u buffer_index=%d old_size=%u dma_free=%u dma_largest=%u reg_raw=%u reg_masked=%u rx_count=%u last_seen=%u last_err=%d last_logical_len=%u last_transfer_arg_len=%u last_counter=%u
```

`requested` and `padded` describe the current allocation before/after existing
block padding. `buffer_index` and `old_size` describe the previous write buffer.
`dma_free` and `dma_largest` remain `MALLOC_CAP_DMA` heap samples after allocation
failure. They are not PSRAM-heap measurements, even when the failed streaming
allocation targeted PSRAM, nor proof that an aligned allocation would fit.
`reg_raw`, `reg_masked`,
and `rx_count` are the current packet-length register, its masked value,
and local RX byte counter.

The remaining fields retain the last failed block read this boot. `last_seen`
is 0/1; when zero, its other fields are unset zeros. `last_err` is the original
return. `last_logical_len` is the original pending length used by the unchanged
counter-advance policy. `last_transfer_arg_len` is the actual `uint16_t` argument
after the existing padding and conversion, not the pre-conversion padded value.
`last_counter` is the local counter before that read's advancement. Successful
reads do not clear this record. None of these lengths measures bytes consumed.
All unsigned fields are uint32 values except the uint16 transfer argument
and boolean `last_seen`; buffer index and error are signed int32 values.

The probe does not log payloads, addresses, IDs, URLs, credentials, or normal
successful frames. The RX owner retains its existing driver mutex, samples
heap information after allocation returns, and prints after heap queries
release their locks. Absence of a record is not evidence of healthy transport.
Helper and packaging tests exercise real component hashing and rejection
boundaries. A host C test compiles the actual streaming allocator section
against allocation stubs, covering the observed growth, reuse, alignment,
legacy profiles, and retained assertion. It does not execute SDMMC DMA.
CI builds the actual driver but does not execute an SDIO
allocation-failure fixture. Hardware failure capture remains a separate
qualification step; remove this temporary patch and packaging mode after
the cause is identified and a repair is qualified.

The official BSP manifest pins `esp_video ~2.0`, while P4-capable
`esp_capture` requires `esp_video ^2.1`. The source-only BSP build bridge uses
exact inspected commit `f0ef9497efce684997ce391edd19733483e250a5` without
patching/copying BSP production source, and resolves maintained esp_video 2.4.1.
The exact commit tarball is the default. Developers who already have the same
checkout may explicitly set `OPENCLAW_TAB5_BSP_LOCAL_PATH`; configuration
rejects it unless HEAD is that commit and the worktree is clean.

Provision from USB with `wifi set <ssid> <passphrase>` and `gateway setup-code
<code>`. Kconfig credentials only seed an unconfigured unit.

### Allocation policy

Tab5 sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`, so ordinary allocations
above 1 KiB prefer PSRAM, including each 2 KiB WebSocket receive/transmit
buffer. This reduces competition for internal memory without changing the
32 KiB internal reserve, task stacks, TLS allocation policy, or SDIO DMA
requirements. The preference can fall back to internal memory; it is not a
hard placement guarantee. The 1 KiB cutoff is a qualification candidate, not
a proven optimum or a confirmed fix for the observed SDIO allocation assertion.

## C6 Wi-Fi prerequisite

The ESP32-C6 must already run firmware compatible with `esp_hosted` 1.4.0 and
`esp_wifi_remote` 0.8.5. The adapter powers `BSP_FEATURE_WIFI` and uses CMD13,
CLK12, D0/1/2/3 11/10/9/8, reset GPIO15 active-low, four-bit 40 MHz SDIO, C6
target. Missing transport is shown as `Wi-Fi coprocessor unavailable`.

## Display and audio

The default home shows the static OpenClaw image, board identity, independent
Wi-Fi/Gateway/Talk status, and error details. Canvas and local Diagnostics take
precedence. Tab5 sets `display.idle_brightness = 18`, keeping the home visible
at a dim idle backlight at the cost of higher idle power; other boards that
omit this field retain zero/off while idle. Explicit off requests are not
clamped. The static home does not imply full touch, camera, network or Talk
hardware qualification.

The maintained MIPI-DSI/LVGL stack rotates to 1280x720 landscape and probes
ILI9881C+GT911, ST7123 (touch firmware 3), and ST7121 (firmware 1). The upstream
August 7, 2026 report records physical verification of ST7123. The September
2026 test campaign instead recorded touch firmware revision 1, selecting
ST7121, with panel initialization and LVGL startup observed. This is not full
display or touch qualification. ST7121 uses the isolated Apache-2.0 panel extension
adapted from M5Stack's official `M5Tab5-UserDemo` commit
`68b19d37fbf9cefd5f256992f5dca34794c62ab4`; touch remains on the maintained
ST7123-compatible API. No S3 bounce-buffer workaround is present.

ST7121 explicitly places its two 720x40 RGB565 draw buffers and PPA rotation
buffer in PSRAM, matching the other panel path. Their configured payload totals
172,800 bytes, excluding allocation overhead; this is not a measured gain in
internal heap or proof of working voice. Buffer dimensions, double buffering,
rotation, cache handling, task stacks, and global allocation policy are unchanged.

The board adapter keeps the official four-slot TDM order: MIC-L, speaker
reference, MIC-R, headset mic. Shared AEC/WakeNet consume MIC-L plus the
far-end reference; dual-mic BSS is intentionally disabled because it starves
the task watchdog's CPU0 idle subscriber on early 360 MHz P4 silicon. Ambient
capture uses the speech-recognition AFE, while active Talk reopens the source in
16 kHz voice-communication mode with WakeNet and redundant device NS/VAD
disabled; AEC and nonlinear echo suppression remain active. Talk and voiceWake
advertise only after media/wake init. Playback and capture use jointly
allocated, reciprocally paired I2S channels with distinct data lines and the
shared 48 kHz, 64-bit frame contract; the ES7210 input policy is 30 dB.
Playback uses 100% codec volume and a +6 dB post-decode PCM16 boost in the shared
renderer. This raises quiet speech but saturates peaks that exceed digital
headroom. The local speaker test compensates for the PCM boost, retaining its
existing digital tone level; the codec-volume increase still applies to it.

## Commands and policy

Commands outside the Gateway's generic platform policy require an explicit
`gateway.nodes.commands.allow` entry. This example's full device-owned allow
set is:

```json
["camera.list","camera.snap","canvas.present","canvas.navigate","canvas.hide","canvas.snapshot","canvas.a2ui.pushJSONL","canvas.a2ui.push","canvas.a2ui.reset","dir.list","file.fetch","file.write","hardware.status","face.set","face.gesture","talk.start","talk.stop"]
```

`camera.snap` is privacy-heavy. The firmware refuses to open the camera unless
the UI controller has visibly armed and flushed its capture indicator, and
camera/Talk media ownership is serialized. Tab5 exposes only its front camera:
use `--facing front`. The firmware rejects `back` instead of duplicating or
mislabeling the front image.

```sh
openclaw nodes camera snap --node <tab5-node> --facing front
```

`maxWidth` is an upper bound, not an exact JPEG width. Rotation and downscaling
use one uniform PPA scale rounded down to a multiple of 1/16, without upscaling.
For the 90/270-degree orientation, `maxWidth: 640` produces 630x1120 pixels;
the JPEG and response report those actual dimensions. Packed RGB length and
PPA output pitch use the same dimensions, while the allocation retains its
cache-line alignment. This geometry correction does not qualify color,
exposure, warm-up or the complete camera pipeline.

### Media stage diagnostics

The capture owner emits one `camera_capture_diag` failure with a fixed `stage`,
`domain` and numeric `error`. `esp` is the BSP result; `errno` is captured
immediately after the failed syscall; `validation` uses zero rather than stale
errno. Existing numeric format, stride and length rejection details remain.
The single `dqbuf_begin` marker precedes the first blocking dequeue; its
presence without a terminal record does not prove capture success or failure.
There is no per-frame trace, new timeout, capture retry or format fallback.
The camera RPC's existing error code and cleanup behavior are unchanged.

Talk uses the [component's fixed-field stage diagnostics](../../components/esp-openclaw-talk/README.md#stage-diagnostics),
with room-generation startup/peer/teardown records and existing audio-counter
snapshots. They separate local progress from RTC/media success and contain no
SDP, broker credentials or session identifiers. Native builds and synthetic
fixtures are not physical camera, RTC or audio qualification.

| Surface | Commands and bounds |
| --- | --- |
| Device | `device.info`, `device.status`, `wifi.status` |
| Talk | `talk.start`, `talk.stop` |
| Canvas | `present`, `navigate`, `hide`, `snapshot`, `a2ui.pushJSONL`, compatibility `a2ui.push`, `a2ui.reset`; no `canvas.eval` |
| A2UI action | Fixed `CANVAS_A2UI` agent request; id/name/surface/component <=64, context <=2 KiB |
| Camera | `camera.list`, `camera.snap --facing front`; fixed 1280x720 RGB565 sensor capture, converted by PPA to rotated/downscaled RGB888 before JPEG encoding, <=1 MP and <=1 MiB |
| Files | `dir.list`, `file.fetch`, `file.write`; canonical `/sd/**`, <=1 MiB, strict base64/SHA-256, no-overwrite default |
| Hardware | `hardware.status`, <=8 KiB typed partial BMI270, RX8130CE, INA226, SD, USB, RS-485 sections |

INA226 is power-path telemetry, not a fuel gauge: battery percentage, battery
health, source, and charging are never fabricated.

## GPIO20 mode and limits

`menuconfig -> M5Stack Tab5 room board -> GPIO20 hardware mode` exclusively
selects `USB_HOST` (default) or RS-485 UART1 half-duplex TX20/RX21/DE34. The
other section reports disabled/conflict. USB inventory never exposes serial
numbers and is capped at eight.

Canvas limits remain 96 components, 256 KiB input/store, depth 8, six images,
2 MiB fetch, 2048 px/side, one decoded megapixel. There is no `camera.clip`
(long-media audio ownership/payload/cancellation is unresolved), `/usb/<n>`
until a mass-storage driver mounts it, raw USB/RS-485 tunnel, arbitrary file
root, reboot/shutdown/power mutation, or continuous sensor/media stream.

File preflights are side-effect free. New no-overwrite files use a temp file
and the filesystem's no-replace rename behavior. FAT replacement cannot rename
over an existing target, so overwrite uses a recoverable transaction (old file
to bounded backup, temp to target, restore on failure) rather than claiming a
fully atomic replacement. A 1 MiB decoded write fits below the node's 2 MiB
assembled-message ceiling after base64/JSON overhead; larger writes are not
supported.
