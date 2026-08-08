# esp-openclaw-node-provisioning

Reusable local provisioning for ESP OpenClaw examples: NVS-backed station
credentials, the USB REPL, setup-code commands, saved-session reconnect, and
the canonical device/Wi-Fi status handlers. Network hardware must be prepared
by the board before `esp_openclaw_node_wifi_start()` is called; this is what
allows the same station helper to run over native Wi-Fi or `esp_wifi_remote`.
