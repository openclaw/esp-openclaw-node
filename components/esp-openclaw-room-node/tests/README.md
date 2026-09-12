# Talk lifetime source proofs

## Console header framing

The lifecycle runner's `console-diagnostics-bytes` and
`console-diagnostics-contention` cases execute the actual `diagnostics status`
handler with synthetic snapshots. They preserve normal tone output, `error/none`,
spaced error labels, counters, newline and subsequent lines. The contention
case schedules another normal stdio record after the first completed print
call: the original split header fails intact-line checks; the single-call
header passes without changing output bytes.

The fixture uses real stdio calls on a temporary stream and a deterministic
between-call schedule, not hardware concurrency. This protects the header
against normal writers using the same stdio stream; it does not make the
multi-line report atomic or serialize ROM output. It does not establish the
cause of a prior observation whose raw header was not retained.

## Media diagnostic regressions

The Talk pthread suite now captures the actual adapter's fixed-field records,
injects HTTP initialization/transport/status/body failures and callback return
errors, and checks first-failure retention, ignored-return behavior, late
cancellation and secret exclusion. The room lifecycle fixture checks actual
startup failure short circuits, ordered teardown stages, SDK log suppression
before negotiation, and existing audio-counter snapshots outside locks.

`python3 -m unittest discover -s scripts/tests -p test_tab5_camera_diagnostics.py -v`
compiles the actual Tab5 capture/cleanup functions with narrow synthetic V4L2
boundaries under ASan/UBSan. It covers immediate errno retention across logging
and failing cleanup, stale errno on validation, partial mappings, initialization
caching, RGB565 variants and bounded dequeue-loop records. This is not V4L2 ABI,
DMA, sensor or physical frame proof. `TAB5_CAMERA_SOURCE` can select a trusted
baseline source file for a red comparison without editing the worktree.

## Static home UI

The UI host runner compiles the real UI controller and board binding with LVGL 9,
ASan and UBSan. Display, timer, Canvas, diagnostics and animated-face boundaries
are synthetic; it does not access a device, network, Gateway or provider.

```sh
python3 components/esp-openclaw-room-node/tests/run_ui_host_tests.py --lvgl-dir "$LVGL"
```

`LVGL` points to existing LVGL 9 sources, such as a configured room example's
`managed_components/lvgl__lvgl`. Optional `--snapshot home.ppm` writes the
synthetic Tab5 framebuffer. CI uses the existing Waveshare build's dependency.
The three cases cover Tab5 idle brightness, zero-default display sleep and the
animated-board path. They check independent connection text, retained facts
after paint lock failure, noninteractive bounded home content, tap/hold
dispatch, Canvas/diagnostics visibility, camera-indicator priority and
brightness, hint expiry, explicit off requests and a nonblank rendered mascot.
Error details remain visible alongside ready connection facts, yield to
Diagnostics, and clear on a later non-error state.
The lifecycle runner also checks real node/operator/network event handling for
the home facts, including missing-session results versus ordinary disconnects,
raw busy/failure results, accepted reconnects and later connection failures.

## Talk lifecycle

These tests compile the real room controller and Talk adapter against synthetic
Node, WebRTC, media, UI and scheduling boundaries. They do not operate hardware
or connect to a Gateway/provider. Production deployment delta is **ZERO**.
Physical pre-fix control-loss reproduction and acoustic teardown verification
remain outstanding; local ownership observations are not audible-continuation
claims.

## Confirmed baseline and candidate

The coordinator ran the actual production baseline
`525f387afe8b08ea91fe1fcea0fcecfb75c7e4ea` on 2026-08-31: **15 cases, 2 failed**,
exit 1, with ASan+UBSan and no sanitizer diagnostics. Both `operator-disconnect`
and `gateway-session-closed` observed exactly:

```text
close=0 release=0 ambient_restore=0 media_owned=1 active=1 peer_disconnects=0
```

Each failed five actual lifecycle invariants. Explicit stop and the other 12
controls passed. This executed confirmation supersedes the earlier worker's
unexecuted repro report. The untouched evidence remains in the coordinator's
original task artifacts. SHA-256 identities:

- `baseline-room.log`: `437e66f6c5667a64d3e959fbfc4f09611eba13c88bbc86c44231f598d79673b4`
- `baseline-confirmed.md`: `6a61590a07053955e4f2cc89abd4460150d1bb887e8ecaca814c5b7d28fafbee`

The coordinator removed only a newly attempted `-Wstrict-prototypes` flag that
rejected two pre-existing pinned SAL typedefs. The repository's existing
`-Wall -Wextra -Werror` gate, extra `-Wmissing-prototypes`, ASan and UBSan were
retained. No vendor or existing warning gate was changed.

The candidate runs all 36 real-controller cases green, including those same baseline regressions. The two
previously red cases now observe:

```text
close=1 release=1 ambient_restore=1 media_owned=0 active=0 peer_disconnects=0
```

The expanded room suite covers 36 cases: authoritative loss at admission, media
wait, pending config/create, and inside start; wake admission; same-handle
reconnect incarnation; displaced owner loss; deliberate origin rebind; exact
terminal closure; malformed, duplicate, mismatched, unrelated and stale events;
recoverable final errors and unverified replacement events;
terminal events before create publishes identity; graceful cancellation before
signaling start; node-role loss;
explicit stop; setup, enqueue, open/provider/start/timer failure; timeout;
10,000 coalesced stop requests; late A create while B is admitted; and replacement
ordering through SDK close, ambient restoration and media release. It
intentionally reuses A's RTC address for B, then delivers old generation peer,
setup-failure and timeout actions. No old action changes B's lifecycle or UI.

## Commands

From the repository root, with the pinned submodules and installed ESP-IDF:

```sh
python3 components/esp-openclaw-room-node/tests/run_lifecycle_host_tests.py --managed-components "$MANAGED"
python3 components/esp-openclaw-room-node/tests/run_lifecycle_host_tests.py --analyze --managed-components "$MANAGED"
python3 components/esp-openclaw-talk/tests/run_host_tests.py --sanitize --cjson-dir "$CJSON"
python3 components/esp-openclaw-talk/tests/run_host_tests.py --analyze --cjson-dir "$CJSON"
python3 components/esp-openclaw-talk/tests/run_threaded_tests.py --cjson-dir "$CJSON"
python3 components/esp-openclaw-talk/tests/run_threaded_tests.py --tsan --cjson-dir "$CJSON"
python3 components/esp-openclaw-talk/tests/run_threaded_tests.py --analyze --cjson-dir "$CJSON"
```

`MANAGED` names an existing configured room example's `managed_components`;
`CJSON` names its `espressif__cjson/cJSON`. The final candidate runs used
`MANAGED=examples/waveshare-esp32-s3-touch-amoled-2.06-room-node/managed_components`
and `CJSON=$MANAGED/espressif__cjson/cJSON`, created by the isolated source build.
Earlier candidate runs used read-only pre-existing dependencies. Both runners
also accept `--webrtc-dir` for an existing read-only checkout of the exact
submodule pin, and `--idf-path` for the installed SDK. The baseline command was
the first room command with explicit `--webrtc-dir` and `--managed-components`
pointing to those read-only dependencies. Machine-specific paths are omitted
here; original evidence is retained unchanged. No private config is needed.
Generated test binaries live in temporary directories, never the source tree.

The existing 15 Talk routing tests remain unchanged. The pthread runner executes
12 lifetime/ownership cases, including 200 concurrent create/failure-vs-seal
iterations. It uses real mutexes and condition-variable barriers, never sleeps
or critical-depth counters as synchronization. Barriers hold admitted ICE,
connected, failure and SDP-answer callbacks while teardown seals and blocks in
the actual drain. Separate barriers hold remote config/create and HTTP replies
while teardown completes and frees SDK/application context. Releasing those
replies must submit no late create and must deliver no callback into freed
context. A late create closes only its original operator/key/voice ID. Immediate
submission failure returns the reserved reference; canceled start and independent
lifecycle setters are also exercised.

The strict ASan+UBSan build and the separate TSan build both passed on the
candidate host. An initial threaded fixture incorrectly required a voice-close
RPC after a failed create (no voice was created); that assertion was corrected
to require cleanup only after successful create. It was a test expectation
failure, not a sanitizer finding. Clang static analysis of the real routing,
threaded and room-controller translation units reported no diagnostics.

`test_config_compat.c` includes the real public Talk and Node headers. It compiles
unchanged positional and designated initializers, compares every field offset,
size and alignment with independent baseline structs, and checks values. On the
64-bit host, Talk is 72 bytes/alignment 8 and Node is 128 bytes/alignment 8.
The same fixture also compiled with the configured ESP32-S3 and ESP32-P4 compilers
and real target headers, retaining all layout assertions and adding `-Wextra -Werror`.
These checks do not shadow either public API or relax missing-field warnings.

CI runs the routing and pthread/ABI suites after the component-test app build,
and the real room suite after the Waveshare build supplies its managed headers.
A host TSan run supplements ASan+UBSan; it does not replace target/hardware proof.

Existing host regressions also run unchanged:

```sh
python3 components/esp-openclaw-room-node/tests/test_room_audio_port_compat.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I components/esp-openclaw-room-node components/esp-openclaw-room-node/room_file_validation.c components/esp-openclaw-room-node/tests/test_room_file_validation.c -o /tmp/talk-room-file-test
/tmp/talk-room-file-test
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I components/esp-openclaw-room-node components/esp-openclaw-room-node/room_diagnostics_metrics.c components/esp-openclaw-room-node/tests/test_room_diagnostics_metrics.c -o /tmp/talk-room-metrics-test
/tmp/talk-room-metrics-test
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I components/esp-openclaw-room-node components/esp-openclaw-room-node/room_pcm_gain.c components/esp-openclaw-room-node/tests/test_room_pcm_gain.c -lm -o /tmp/talk-room-gain-test
/tmp/talk-room-gain-test
```

All four passed. The compiler warnings and sanitizer gates were preserved.

## Boundary and limits

Room scheduling is deterministic and single-threaded. Its queue and media hooks
exercise the actual owner code; they are not a second implementation of the
lifecycle. The isolated pthread runner provides the true concurrent admission
and drain proof for the reusable callback boundary. The room fake copies the
SDK's pointer-sized prepared-call binding and uses real Talk/Node/WebRTC/peer,
capture, renderer, codec, LVGL, SAL, ESP error, console, timer and HTTP headers.
It also uses the real ESP-IDF heap header. Unreferenced board startup and physical
media code are omitted by linker section collection, but GCC ASan global
registration retains the static console command table and its diagnostic
callbacks. The console cases explicitly enable typed tone, UI, Canvas, heap,
clock and `strlcpy` snapshot fakes; unsupported calls still abort with the symbol
name outside those cases. The real controller and Talk lifecycle remain linked
and exercised. All fixtures use synthetic identities and data.

The room runner also checks its queue fixture before running the 36 lifecycle
cases. Allocation dimensions, item counts and byte arithmetic use `size_t`;
allocation overflow is rejected before allocation, and copy extents are checked
against the allocated storage. The queue regression uses the fixture's own
helpers to check arithmetic above `UINT_MAX` and at the `SIZE_MAX` boundary
without large allocations, then exercises actual FIFO copies, compaction, full
queue rejection and reuse with small buffers. Separate subprocesses must abort
on an unsupported diagnostics call and on an extent beyond allocated storage.
`--analyze` covers the fixture and queue regression as well as the controller.

The original published fixture's link failure was reproduced in a local Linux
AArch64 container with Debian GCC 12.2.0 and ASan+UBSan. Object relocations showed
the retention chain from `.init_array` through the ASan global metadata to the
console `COMMANDS` table and `diagnostics_console_command`. The corrected fixture
passed all 36 lifecycle cases and the queue/boundary checks with the same GCC,
warning, sanitizer and section-collection flags. This is local-container host
proof; the published PR's CI and CodeQL gates still require a new run after the
repair is reviewed and pushed.

The Node API already supplies source handle and ordered, transport-filtered
callbacks. The room freezes its connection incarnation at admission; no Node
production changes or new callback/config fields are needed. Destroying an
operator with queued RPCs is unsupported: reconnect must reuse that Node until
cleanup references finish. Actual SDK peer joins, remote transport timing,
capture restoration failures, and audible output still require separate
physical proof. Host ASan/TSan cannot validate those hardware/library internals.

## Source-only firmware build

The Waveshare ESP32-S3 example built successfully using installed ESP-IDF 5.5.5,
its existing Python 3.13 environment, the exact repository submodule pins, and
public `sdkconfig.defaults`. The first environment activation failed because
PATH selected Python 3.14 and no matching IDF virtual environment existed;
selecting the already-installed IDF Python 3.13 environment resolved it without
installing or changing the SDK.

After activating that environment, the build command was:

```sh
idf.py -C examples/waveshare-esp32-s3-touch-amoled-2.06-room-node -B /tmp/talk-source-firmware -DIDF_TARGET=esp32s3 -DSDKCONFIG=/tmp/talk-source-firmware/sdkconfig build
```

The full app image is `0x451ed0` bytes, with `0x3ae130` bytes (46%) free in the
unchanged `0x800000` app partition. Both changed production files compiled;
no firmware compiler warnings or errors were emitted. Size gates were unchanged.
The target ABI compile reused the Talk entry in `compile_commands.json`, changed
only the source/output to the compatibility fixture and a task-local object,
and added `-UNDEBUG -Wextra -Werror`. No target executable was launched.

The coordinator also built the Tab5 ESP32-P4 example with the same SDK and
public defaults, using a separate temporary build/config directory. Its image
is `0x595370` bytes, with `0x26ac90` bytes (30%) free in the unchanged `0x800000`
app partition. One warning came from the unchanged managed `esp-dl` JPEG encoder
(a missing `pixel_reverse` initializer); no vendor patch was made. Both changed
production files and the P4 real-header ABI fixture compiled successfully.

The coordinator independently repeated all host suites, both threaded sanitizer
modes and static analysis. Codex autoreview found no blockers at its configured
P0 threshold; it did not execute tests. These are two source-only target builds,
not the complete five-target remote CI matrix or physical proof. Both task-local
vendor checkouts remain clean. All build/config/generated dependency artifacts
are ignored or outside the source tree. No private config, flash contents, NVS or
recordings were copied. No hardware, USB/serial, pairing, network service tests,
deployment, commits, pushes or publication occurred.

## Hardware qualification

This is a checklist for a new physical run, not additional evidence for the
historical results above. Start every row at `not run`; change it to `pass` or
`fail` only after observing the named outcome. Keep untested or inapplicable
rows at `not run` with a reason. A source build, host test, successful pairing,
or renderer acceptance alone does not qualify the device.

### Record the candidate

- Board model, silicon and display/touch revision; for Tab5, the C6 firmware
  version and selected USB-host or RS-485 mode.
- Firmware source commit and image SHA-256, example and build configuration,
  ESP-IDF version, and submodule commits. For a registry package or prebuilt
  image, also record its version, archive/image digest, and advertised source
  identity; do not substitute the current checkout's identity.
- Gateway version and source commit when available, selected role scopes and
  command allowlist, test date, and an operator-selected non-identifying unit
  label.

Use only the assigned device and serial port with an isolated or explicitly
approved Gateway. Get approval before power cycling, interrupting a connection,
capturing an image, or starting a provider-backed Talk call. Preserve pairing
state during reconnect checks; do not erase NVS or disturb other clients.

### Run the checks

Follow the selected [Waveshare](../../../examples/waveshare-esp32-s3-touch-amoled-2.06-room-node/README.md)
or [Tab5](../../../examples/m5stack-tab5-room-node/README.md) build, provisioning,
command-policy, and board-specific prerequisites first.

| Check | Procedure and required observation | Result |
| --- | --- | --- |
| Boot and provisioning | Boot the selected image, provision through the documented console, and confirm the intended node and operator connections. Record initialization failures rather than treating a partially available node as fully qualified. | not run |
| Saved-session reconnect | Power cycle the assigned board without clearing NVS. Confirm both roles reconnect to the intended Gateway without a new setup code; record any approval or credential failure. | not run |
| Status commands | Run the three commands below. Confirm successful responses for the intended node and plausible device/Wi-Fi data, not merely a connected entry in `nodes status`. | not run |
| Display and Canvas | Check orientation, touch alignment, show/hide, a bounded image, and a supported A2UI button through the documented node commands. Confirm the button reaches its owning agent session. This does not qualify HTML rendering or unsupported interactive controls. | not run |
| Local audio | Run `diagnostics status` and `diagnostics tone` while Talk is idle. Inspect fresh microphone/capture data and have the operator confirm the tone is audible; renderer acceptance alone is insufficient. | not run |
| Talk and recovery | Start one call with the documented `wake` console command. Confirm audible bidirectional speech and normal stop. On the approved test connection, exercise operator control loss or matching session closure; confirm playback ends, media ownership is released, and ambient capture resumes before another call. Record audible continuation or stuck ownership as failures. | not run |
| Tab5 camera | With explicit capture consent, run the documented front-camera snap. Observe the visible capture indicator and inspect the returned image. Do not infer rear-camera, clip, or external USB-camera support. | not run |
| Tab5 hardware status | Invoke `hardware.status` and check the installed sensors and selected interface mode. Missing peripherals must remain visibly unavailable/partial; do not infer battery percentage from INA226 telemetry. | not run |

Gateway-side status checks:

```sh
openclaw nodes status --json
openclaw nodes invoke --node <node-id> --command device.info --json
openclaw nodes invoke --node <node-id> --command device.status --json
openclaw nodes invoke --node <node-id> --command wifi.status --json
```

Room-node serial console checks:

```text
diagnostics status
diagnostics tone
wake
```

Record observations, failures, and evidence references alongside each result,
including the duration of any sustained Talk run. Keep loudness/distortion,
echo cancellation, wake false accepts, thermals, and long-running stability
unqualified unless separately measured on that board and image.

Before sharing evidence, remove credentials, setup codes, private URLs and
addresses, network identifiers, personal paths, and device identifiers. Do not
publish NVS contents or recordings as routine proof. Preserve the historical
source-only and panel-specific caveats; a result for one unit or firmware does
not qualify another.
