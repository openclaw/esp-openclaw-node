# esp-openclaw-ui

Small shared LVGL widgets for the independent Watch and Dial interfaces.
Callers own hardware sampling, state, placement, and the display lock.

The 94 × 22 status strip shows Wi-Fi signal, the OpenClaw link, and an unknown
battery outline with a dash. Disconnected Wi-Fi and Gateway links are crossed
out independently. A connected radio with unavailable RSSI has a lit dot and
neutral arcs. Valid RSSI lights one arc below −75 dBm, two from −75 through
−61 dBm, and three at −60 dBm or above. These are display bands, not a throughput
estimate. There is no battery percentage or charging claim without a provider.

`openclaw_setup` supplies a local phone-setup presenter for the existing setup
runtime. It shows discovery QR/manual details, comparison, operation progress,
errors and USB alternatives. This component alone adds no board entry or BLE
advertising; Watch and Dial adapters must create the runtime and route local
intents to it. QR display uses the existing `LV_USE_QRCODE` option; builds
without it offer manual details.

Create input drivers before the view. Hold the display lock for every call and
destroy the view before its parent or input drivers. `show` wraps existing
driver reads without changing driver/user data; `close` restores them. The
bounded action callback delegates to the canonical runtime. OPEN returns its
admitted request ID through the key before returning success, allowing an
immediate Close to cancel without waiting for a UI refresh. Snapshot delivery
must preserve the runtime's boot lifetime. Queue failures remain visible and
require an explicit retry.

Comparison confirmation uses the exact request/generation/peer. It first
renders the comparison with `lv_refr_now`, invalidates that comparison once,
then waits for `LV_EVENT_FLUSH_WAIT_FINISH` to establish that the previous
final DMA completed. Only new released driver samples after that event arm
confirmation. A subsequent physical press/release inside Codes match, or a
focused encoder/Enter press/release, can confirm. Programmatic widget events
cannot confirm. Watch adapters must also deliver fresh BOOT samples and
suppress their normal Canvas toggle while consumed. BOOT cancels. The first
press on a dark display only wakes it.

Before leaving, covering or replacing the setup view, call `close` to revoke
immediately. Input replacement also closes the view. Discovery buffers and
canvas pixels are cleared; the retained LVGL QR payload is overwritten through
its public API. LVGL's allocator and encoding temporaries provide no secure
erase guarantee. Never log or remotely expose these local views, or capture
real setup credentials in media.

`tests/run_setup_host_tests.py --lvgl-dir /path/to/lvgl` exercises real LVGL
flushes and pointer/encoder reads under ASan/UBSan at Watch and Dial sizes.
It proves the presentation boundary; physical panel completion, phone scanning
and Studio exchanges remain device-test requirements.
