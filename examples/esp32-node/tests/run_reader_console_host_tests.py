#!/usr/bin/env python3
"""Check secret-line ownership with the production compact console handler."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", required=True, type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="reader-console-") as directory:
        work = Path(directory)
        (work / "freertos").mkdir()
        (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\nconst char *esp_err_to_name(esp_err_t);\n")
        (work / "esp_console.h").write_text('#pragma once\n#include "esp_err.h"\ntypedef struct {const char *command, *help; int (*func)(int,char **);} esp_console_cmd_t;\nesp_err_t esp_console_cmd_register(const esp_console_cmd_t *);\n')
        (work / "esp_openclaw_reader.h").write_text('#pragma once\n#include <stdbool.h>\n#include "cJSON.h"\n#include "esp_err.h"\n#define ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH 1024\nesp_err_t esp_openclaw_reader_setup_code(const char *);\ncJSON *esp_openclaw_reader_status(void);\n')
        (work / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        (work / "freertos/FreeRTOS.h").write_text("#pragma once\n#define pdMS_TO_TICKS(x) (x)\n")
        (work / "freertos/task.h").write_text("#pragma once\nvoid vTaskDelay(unsigned);\n")
        binary = work / "console-test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-Wno-deprecated-declarations",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(work), "-I", str(args.cjson_dir),
                        str(tests / "test_reader_console.c"), str(args.cjson_dir / "cJSON.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
