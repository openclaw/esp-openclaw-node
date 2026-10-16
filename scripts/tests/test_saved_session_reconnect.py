"""Exercise the real reconnect worker when request admission fails transiently."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class SavedSessionReconnectTest(unittest.TestCase):
    def test_request_admission_retries_without_an_event(self):
        root = Path(__file__).resolve().parents[2]
        source = root / "components/esp-openclaw-node-provisioning/esp_openclaw_node_example_saved_session_reconnect.c"
        with tempfile.TemporaryDirectory(prefix="node-reconnect-") as directory:
            work = Path(directory)
            (work / "freertos").mkdir()
            (work / "esp_openclaw_node_example_saved_session_reconnect.h").write_text(
                """
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *esp_openclaw_node_handle_t;
typedef void *TaskHandle_t;
typedef int esp_openclaw_node_event_t;
typedef struct { void *node; void *task; } esp_openclaw_node_example_saved_session_reconnect_t;
typedef struct { int source; const char *gateway_uri; const char *value; } esp_openclaw_node_connect_request_t;
typedef struct { int reason; } esp_openclaw_node_connect_failed_event_t;
typedef struct { int reason; } esp_openclaw_node_disconnected_event_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION 1
#define ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED 1
#define ESP_OPENCLAW_NODE_EVENT_DISCONNECTED 2
#define ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED 1
#define ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST 2
#define ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST 1
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGW(...) ((void)TAG)
uint32_t ulTaskNotifyTake(int, uint32_t);
bool esp_openclaw_node_wifi_wait_for_connection(uint32_t);
void vTaskDelay(uint32_t);
esp_err_t esp_openclaw_node_request_connect(void *, const esp_openclaw_node_connect_request_t *);
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, int, void **);
void xTaskNotifyGive(void *);
"""
            )
            for name in ("esp_openclaw_node_wifi.h", "esp_log.h", "freertos/FreeRTOS.h", "freertos/task.h"):
                (work / name).write_text("")
            fixture = work / "main.c"
            fixture.write_text(
                '#include "esp_openclaw_node_example_saved_session_reconnect.h"\n'
                '#include <assert.h>\n#include <setjmp.h>\n'
                f'#include "{source}"\n' + r"""
static jmp_buf idle;
static int calls, waits, failure;
static void (*worker)(void *);
static void *worker_arg;
uint32_t ulTaskNotifyTake(int clear, uint32_t timeout) {
    assert(clear == pdTRUE && timeout == portMAX_DELAY);
    if (waits++) longjmp(idle, 1);
    return 1;
}
bool esp_openclaw_node_wifi_wait_for_connection(uint32_t timeout) {
    assert(timeout == portMAX_DELAY); return true;
}
void vTaskDelay(uint32_t delay) { assert(delay >= 1000); }
esp_err_t esp_openclaw_node_request_connect(void *node, const esp_openclaw_node_connect_request_t *request) {
    assert(node != NULL && request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION);
    assert(++calls <= 2);
    return calls == 1 ? failure : ESP_OK;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int priority, void **out) {
    (void)name; (void)stack; (void)priority;
    worker = fn; worker_arg = arg; *out = arg; return pdPASS;
}
void xTaskNotifyGive(void *task) { assert(task != NULL); }
int main(void) {
    int outcomes[] = {ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE, ESP_OK};
    for (unsigned i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        esp_openclaw_node_example_saved_session_reconnect_t state = {0};
        calls = waits = 0; failure = outcomes[i];
        assert(esp_openclaw_node_example_saved_session_reconnect_start(&state, &state, NULL) == ESP_OK);
        if (!setjmp(idle)) worker(worker_arg);
        assert(calls == (failure == ESP_ERR_NO_MEM ? 2 : 1));
    }
    return 0;
}
"""
            )
            binary = work / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work), str(fixture), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
