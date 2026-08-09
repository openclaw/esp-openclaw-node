# esp_openclaw_node_unity_tests

This is the ESP-IDF Unity test app for the `esp-openclaw-node` component.

Files:

- `CMakeLists.txt`: test app project file
- `main/test_esp_openclaw_node.c`: test cases

## What these tests do

The test app runs on the target and executes `unity_run_all_tests()` from
`app_main()`.

Each test case resets NVS first. The suite focuses on component logic that does
not require a live OpenClaw gateway.

## Current coverage

- persisted reconnect-session validation, persistence, reload, and clearing
- identity seed persistence and auth-payload signing
- node-config ownership for runtime-provided TLS PEM data
- setup-code validation for malformed or ambiguous payloads
- explicit connect-request argument validation for saved-session, token,
  password, and no-auth sources
- auth-material selection, including the rule that password auth is not included
  in the device-signature payload
- destroy-path notification safety
- transport-state edge cases around challenge ping, clean close, and disconnect
  rejection while still connecting
- Gateway-owned Talk negotiation, strict descriptor rejection, bounded close,
  late-create cancellation, and second-call recovery

## Build and run

Example for `esp32s3`:

```bash
idf.py -C components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests set-target esp32s3
idf.py -C components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests build
idf.py -C components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests -p /dev/ttyACM0 flash monitor
```

The suite runs automatically at boot. A passing run ends with a summary like:

```text
-----------------------
18 Tests 0 Failures 0 Ignored
OK
```
