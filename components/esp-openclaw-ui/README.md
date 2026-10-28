# esp-openclaw-ui

Small shared LVGL widgets for the independent Watch and Dial interfaces.
Callers own hardware sampling, state, placement, and the display lock.

The 94 × 22 status strip shows Wi-Fi signal, the OpenClaw link, and an unknown
battery outline with a dash. Disconnected Wi-Fi and Gateway links are crossed
out independently. A connected radio with unavailable RSSI has a lit dot and
neutral arcs. Valid RSSI lights one arc below −75 dBm, two from −75 through
−61 dBm, and three at −60 dBm or above. These are display bands, not a throughput
estimate. There is no battery percentage or charging claim without a provider.
