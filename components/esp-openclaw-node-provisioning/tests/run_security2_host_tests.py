#!/usr/bin/env python3
"""Exercise the production Security2 guard against the real IDF public ABI."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    component = tests.parent
    security = args.idf_path / "components/protocomm/include/security"
    if not (security / "protocomm_security.h").is_file():
        parser.error("--idf-path must provide the actual protocomm public security headers")
    with tempfile.TemporaryDirectory(prefix="setup-security2-") as directory:
        work = Path(directory)
        (work / "sdkconfig.h").write_text("")
        (work / "esp_err.h").write_text(
            "#pragma once\ntypedef int esp_err_t;\n"
            "enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM };\n"
        )
        (work / "esp_event.h").write_text("#pragma once\n#define ESP_EVENT_DECLARE_BASE(name) extern const char *name\n")
        (work / "esp_log.h").write_text("#pragma once\n#define ESP_LOG_NONE 0\nvoid esp_log_level_set(const char *, int);\nint esp_log_level_get(const char *);\n")
        (work / "soc").mkdir()
        (work / "soc/soc_caps.h").write_text("")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text("")
        (work / "freertos/portmacro.h").write_text(
            "#pragma once\n#include <pthread.h>\n"
            "typedef pthread_mutex_t portMUX_TYPE;\n"
            "#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n"
            "#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)\n"
            "#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)\n"
        )
        native = {
            "CONFIG_IDF_TARGET_ESP32S3": 1, "SOC_WIFI_SUPPORTED": 1, "SOC_BLE_SUPPORTED": 1,
            "CONFIG_ESP_WIFI_REMOTE_ENABLED": 0, "CONFIG_ESP_HOST_WIFI_ENABLED": 0,
            "CONFIG_BT_ENABLED": 1, "CONFIG_BT_NIMBLE_ENABLED": 1,
            "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2": 1, "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL": 1,
            "CONFIG_LOG_TAG_LEVEL_IMPL_NONE": 0,
        }
        flags = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                 "-I", str(work), "-I", str(security), "-I", str(component / "include")]
        guard = work / "guard.o"
        binary = work / "security2-test"
        profiles = [None, "CONFIG_IDF_TARGET_ESP32S3", "SOC_WIFI_SUPPORTED", "SOC_BLE_SUPPORTED",
                    "CONFIG_ESP_WIFI_REMOTE_ENABLED", "CONFIG_ESP_HOST_WIFI_ENABLED", "CONFIG_BT_ENABLED",
                    "CONFIG_BT_NIMBLE_ENABLED", "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2",
                    "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL", "CONFIG_LOG_TAG_LEVEL_IMPL_NONE"]
        for excluded in profiles:
            config = native.copy()
            if excluded:
                config[excluded] = 1 - config[excluded]
            defines = [f"-D{key}={value}" for key, value in config.items()]
            subprocess.run([*flags, *defines, "-Dfree=security_test_free", "-c",
                            str(component / "esp_openclaw_setup_security2.c"), "-o", str(guard)], check=True)
            subprocess.run([*flags, str(tests / "test_setup_security2.c"), str(guard),
                            str(component / "esp_openclaw_setup_session.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary), *(["unsupported"] if excluded else [])], check=True)


if __name__ == "__main__":
    main()
