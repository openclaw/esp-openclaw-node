# Waveshare ESP32-S3 Touch AMOLED 1.75C room node — findings

Summary of changes and observations while getting the 1.75C room-node
example running, based on comments in
[openclaw/esp-openclaw-node#92](https://github.com/openclaw/esp-openclaw-node/issues/92)
and subsequent runtime testing.

## Changes from earlier comments (issue #92)

- Switched display lock handling to the 1.75C BSP API.
  `bsp_display_lock()` returns `esp_err_t` (via `esp_lv_adapter_lock`),
  not `bool`. The original boolean test caused the `ESP_ERR_TIMEOUT`
  abort inside `room_canvas_init()`.
- Set `CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=8` in `sdkconfig.defaults`
  (BSP LVGL buffer height). The DMA partial buffers used by this adapter
  are sized independently by `WAVESHARE_DRAW_ROWS` (see below).
- Switched to a custom certificate bundle
  (`DEFAULT_NONE` + `CUSTOM_CERTIFICATE_BUNDLE_PATH` pointing at
  `certificates/public-roots.pem`).
- Removed the redundant `espressif__esp_lvgl_port` dependency from
  `idf_component.yml` and the component `REQUIRES`.
- Kept the 1.75C adapter structure (display start/setup callbacks,
  audio open with ES8311/ES7210, boot button task).

## Instrumentation

- Heap snapshots at key points (`app_main`, before/after
  `bsp_display_start`, before/after audio open, after
  `esp_openclaw_room_node_start`) logging internal and PSRAM free +
  largest block.
- `#define WAVESHARE_DRAW_ROWS` controlling the size of the two partial
  DMA draw buffers (allocated `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).
  The CO5300 QSPI panel cannot DMA from PSRAM, so these must stay
  internal.

**Buffer sizing note:** the numbers in the tables below were measured
with `WAVESHARE_DRAW_ROWS` set to 4 / 8 / 16. That macro is what sizes
the two internal DMA buffers allocated in `waveshare_display_start()`.
`CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT` (currently 8 in
`sdkconfig.defaults`) is the BSP's own LVGL buffer height and is
independent of the adapter's DMA buffers.

## Animated face = true

With the face enabled (`animation_frame_ms = 16`), the LVGL worker
monopolizes CPU 0 long enough to starve `IDLE0` and trip the task
watchdog (default 5 s). Reproduced with both `WAVESHARE_DRAW_ROWS = 4`
and `= 8`. The backtrace is entirely inside the LVGL refresh path
(`lv_display_refr_timer` → `refr_invalid_areas` → object/arc drawing).
Disabling the face removes the watchdog.

## Heap stages vs. DRAW_ROWS (real audio path, face off)

Numbers are approximate from successive runs. DMA buffers sized by
`WAVESHARE_DRAW_ROWS`.

| Stage                   | DRAW_ROWS=4           | DRAW_ROWS=8              | DRAW_ROWS=16      |
|-------------------------|-----------------------|--------------------------|-------------------|
| `app_main`              | 218 KB / 102 KB       | 218 KB / 102 KB          | 218 KB / 102 KB   |
| Before display start    | 201 KB / 102 KB       | 201 KB / 102 KB          | 201 KB / 102 KB   |
| After display buffers   | 161 KB / 69 KB        | 154–161 KB / 65–69 KB    | 139 KB / 49 KB    |
| After audio open        | 91 KB / 47 KB         | 84–91 KB / 43–47 KB      | 69 KB / 32 KB     |
| After `room_node_start` | 21.6 KB / 12.8 KB     | 14–22 KB / 7.6–12.8 KB   | 9 KB / 7.6 KB     |

The display buffers scale roughly linearly with row count. The large
drop happens inside `esp_openclaw_room_node_start()` (node client, NVS,
room structures, etc.).

Even at 4 rows the post-`room_node_start` largest internal block is only
~12.8 KB — not enough for the operator WebSocket task.

### Errors by configuration (real audio)

| Configuration           | Observed failure |
|-------------------------|------------------|
| Face on, any row count  | Task watchdog: LVGL worker monopolizes CPU 0, `IDLE0` starved. Backtrace inside `lv_display_refr_timer` / `refr_invalid_areas`. |
| Face off, DRAW_ROWS=4   | Operator client fails: `ESP_ERR_NO_MEM` then `Error create websocket task`. Node handshake succeeds. Early `esp_lv_adapter_lock` timeout already appears before audio open. |
| Face off, DRAW_ROWS=8   | Same operator-client `ESP_ERR_NO_MEM` / `Error create websocket task`. Node handshake succeeds. |
| Face off, DRAW_ROWS=16  | Operator client fails earlier/more reliably with the same `NO_MEM` / websocket-task error. |

In all face-off cases the node WebSocket connects and completes the
gateway handshake. The operator client then cannot create its task
because the remaining internal largest block (~7.6–12.8 KB) is too
small. LVGL lock timeouts appear throughout once memory is tight.

Typical log with real audio (face off):

```
E (...) openclaw_room: start_operator_client(759): operator client create
W (...) openclaw_room: operator client start failed: ESP_ERR_NO_MEM (retrying)
E (...) websocket_client: Error create websocket task
W (...) esp_openclaw_node: connect_diag stage=failed role=operator reason=0 local_err=-1(ESP_FAIL)
```

## Audio stub A/B (DRAW_ROWS 8 vs 4)

To isolate the audio path’s internal-RAM cost the real
`waveshare_audio_open` was replaced with a minimal stub. The stub used
for these measurements returned `ESP_OK` with dummy handles; the current
code returns `ESP_ERR_NOT_SUPPORTED` and leaves the handles NULL so the
room node takes a safe canvas-only fallback (no undefined behaviour on
`esp_codec_dev_set_in_gain`).

### Stub (current safe version)

```c
static esp_err_t waveshare_audio_open_stub(void *ctx,
                                           esp_openclaw_room_audio_handles_t *handles)
{
    (void)ctx;
    if (handles != NULL) {
        handles->record = NULL;
        handles->playback = NULL;
    }
    log_heap("audio stub (no I2S/codec, canvas-only fallback)");
    return ESP_ERR_NOT_SUPPORTED;
}
```

The stub is selected by `WAVESHARE_AUDIO_STUB` (default 1). A Kconfig
option would be preferable long-term so the WIP default is harder to
miss.

Face stayed off for both runs. The stub causes the expected canvas-only
fallback — fine for the heap measurement.

### Results (stub, face off)

| Stage                 | DRAW_ROWS=8                     | DRAW_ROWS=4                     |
|-----------------------|---------------------------------|---------------------------------|
| After display buffers | free 155 KB / largest 65 KB     | free 163 KB / largest 74 KB     |
| After room_node_start | free ~70 KB / largest **32 KB** | free ~77 KB / largest **37 KB** |
| Dual WebSocket        | both connected                  | both connected                  |
| LVGL lock failures    | rare                            | more frequent                   |

With the audio path removed, both buffer sizes leave enough contiguous
internal RAM for the operator client. `DRAW_ROWS=8` is the cleaner of
the two: the extra ~5 KB of largest block at 4 rows comes with
noticeably more `esp_lv_adapter_lock` timeouts (more partial flushes →
more lock contention while the two clients are coming up).

## Takeaway

- The dual-client path is viable on the 1.75C once the audio pipeline
  is out of the picture.
- The remaining pressure is the combination of the 466×466 DMA draw
  buffers + the permanent internal allocations inside
  `esp_openclaw_room_node_start` / the two node clients.
- Real audio (I2S + ES8311/ES7210 + AFE) consumes enough additional
  internal RAM that the operator client no longer fits at any tested
  draw-buffer size.
- Face-on is a separate scheduling problem (CPU 0 monopolization), not
  the operator `NO_MEM`.
- Talk / WakeNet have not been demonstrated on this board.

For a first upstream slice, **1.75C adapter + face off + DRAW_ROWS=8 +
documented heap ceiling** looks reasonable. Dual node+operator+audio
remains under qualification until permanent internal allocations in the
room node (or a supported way to place more of the WebSocket/client
stacks in PSRAM) are reduced.
