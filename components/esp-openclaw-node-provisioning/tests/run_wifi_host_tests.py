#!/usr/bin/env python3
"""Exercise Wi-Fi status through the production station and command owners."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    component = tests.parent
    with tempfile.TemporaryDirectory(prefix="wifi-status-") as directory:
        work = Path(directory)
        for name in (
            "esp_err.h", "esp_wifi.h", "esp_check.h", "esp_event.h", "esp_log.h",
            "esp_netif.h", "esp_app_desc.h", "esp_system.h", "esp_timer.h",
            "esp_openclaw_node.h", "freertos/FreeRTOS.h", "freertos/event_groups.h",
            "freertos/semphr.h", "soc/soc_caps.h",
        ):
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "wifi_host_sdk.h"\n')
        binary = work / "wifi-status-test"
        cjson_object = work / "cjson.o"
        subprocess.run(["cc", "-std=gnu11", "-g", "-fsanitize=address,undefined",
                        "-Wno-deprecated-declarations", "-c", str(args.cjson_dir / "cJSON.c"),
                        "-o", str(cjson_object)], check=True)
        command = [
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(work), "-I", str(tests / "host"),
            "-I", str(component / "include"), "-I", str(args.cjson_dir),
            str(tests / "test_wifi_status.c"),
            str(component / "esp_openclaw_node_wifi.c"),
            str(component / "esp_openclaw_node_common_device_node_cmd.c"),
            str(component / "esp_openclaw_node_example_json.c"),
            str(cjson_object), "-lm", "-o", str(binary),
        ]
        for flags in ([], ["-DSOC_WIFI_SUPPORTED=0"],
                      ["-DCONFIG_ESP_WIFI_REMOTE_ENABLED=1"],
                      ["-DCONFIG_ESP_HOST_WIFI_ENABLED=1"]):
            subprocess.run(command + flags, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
