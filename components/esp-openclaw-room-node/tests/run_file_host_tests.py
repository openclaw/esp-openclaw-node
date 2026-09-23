#!/usr/bin/env python3
"""Run registered file commands against production C and a temporary host filesystem."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=Path(os.environ.get("IDF_PATH", Path.home() / "esp-idf")))
    parser.add_argument("--mbedtls-dir", type=Path)
    parser.add_argument("--cjson-dir", type=Path, default=repo / "components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests/managed_components/espressif__cjson/cJSON")
    scenarios = ["root-loss", "root-loss-nested", "preflight", "boundaries", "pagination"]
    parser.add_argument("--filter", choices=scenarios)
    args = parser.parse_args()
    mbedtls = (args.mbedtls_dir or args.idf_path / "components/mbedtls/mbedtls").resolve()
    cjson = args.cjson_dir.resolve()
    for source in [mbedtls / "library/base64.c", cjson / "cJSON.c"]:
        if not source.is_file():
            parser.error(f"Missing {source}; provide ESP-IDF mbedTLS and configured cJSON sources")
    with tempfile.TemporaryDirectory(prefix="room-files-host-") as temporary:
        directory = Path(temporary)
        (directory / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_CRC 0x109
""")
        (directory / "esp_check.h").write_text("""#pragma once
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { \\
    esp_err_t err = (expr); if (err != ESP_OK) return err; \\
} while (0)
""")
        (directory / "file_crypto_config.h").write_text(
            "#define MBEDTLS_BASE64_C\n#define MBEDTLS_SHA256_C\n")
        (directory / "file_host_compat.h").write_text("""#include <stddef.h>
#include <strings.h>
size_t strlcpy(char *, const char *, size_t);
""")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-D_XOPEN_SOURCE=700",
                   "-Dlstat=room_file_test_lstat",
                   "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-ftrivial-auto-var-init=pattern",
                   '-DMBEDTLS_CONFIG_FILE="file_crypto_config.h"',
                   "-include", str(directory / "file_host_compat.h")]
        for include in [directory, tests.parent, repo / "components/esp-openclaw-node/include",
                        cjson, mbedtls / "include", mbedtls / "library"]:
            command += ["-I", str(include)]
        binary = directory / "file-tests"
        command += [str(tests / "test_room_files.c"), str(tests.parent / "room_files.c"),
                    str(tests.parent / "room_file_validation.c"), str(cjson / "cJSON.c")]
        command += [str(mbedtls / "library" / name) for name in
                    ["base64.c", "sha256.c", "platform_util.c", "constant_time.c"]]
        command += ["-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        for scenario in ([args.filter] if args.filter else scenarios):
            workdir = directory / scenario
            workdir.mkdir()
            subprocess.run([str(binary), scenario, str(workdir)], check=True, timeout=60)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
