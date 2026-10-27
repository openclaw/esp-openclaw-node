#!/usr/bin/env python3
"""Exercise the production reader lifecycle with controllable SDK transports."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, required=True)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    with tempfile.TemporaryDirectory(prefix="dial-reader-") as directory:
        work = Path(directory)
        (work / "freertos").mkdir()
        (work / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
""")
        (work / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <stdint.h>
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define pdPASS 1
""")
        (work / "freertos/task.h").write_text("""#pragma once
#include "FreeRTOS.h"
int xTaskCreate(void (*fn)(void *), const char *, uint32_t, void *, int, TaskHandle_t *);
uint32_t ulTaskNotifyTake(int, uint32_t);
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelay(uint32_t);
""")
        (work / "freertos/semphr.h").write_text("""#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t, uint32_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
""")
        binary = work / "reader-test"
        cjson_object = work / "cjson.o"
        subprocess.run(["cc", "-std=gnu11", "-g", "-fsanitize=address,undefined",
                        "-Wno-deprecated-declarations", "-c", str(args.cjson_dir / "cJSON.c"),
                        "-o", str(cjson_object)], check=True)
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(work), "-I", str(tests.parent / "main"),
            "-I", str(root / "components/esp-openclaw-node/include"),
            "-I", str(args.cjson_dir), str(tests / "test_dial_reader.c"),
            str(tests.parent / "main/dial_reader.c"),
            str(cjson_object), "-lm", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
