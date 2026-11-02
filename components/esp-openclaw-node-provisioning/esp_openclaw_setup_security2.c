#include "esp_openclaw_setup_security2.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_IDF_TARGET_ESP32S3 && SOC_WIFI_SUPPORTED && SOC_BLE_SUPPORTED && \
    !CONFIG_ESP_WIFI_REMOTE_ENABLED && !CONFIG_ESP_HOST_WIFI_ENABLED && \
    CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && \
    CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2 && CONFIG_LOG_DYNAMIC_LEVEL_CONTROL && \
    !CONFIG_LOG_TAG_LEVEL_IMPL_NONE

#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "protocomm_security2.h"
#include "esp_openclaw_setup_frame.h"

typedef struct {
    protocomm_security_handle_t sdk;
    uint32_t session_id;
    uint64_t peer;
    bool open, revoked;
} security_t;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static security_t *current;
static uint64_t last_peer; /* Boot lifetime: never reset by init/cleanup. */

static void discard(void *bytes, size_t size)
{
    volatile uint8_t *cursor = bytes;
    for (size_t i = 0; bytes && i < size; ++i) cursor[i] = 0;
    free(bytes);
}

bool esp_openclaw_setup_security2_quiet_logs(void)
{
    const char *tags[] = {"security2", "srp6a", "protocomm", "protocomm_nimble", "protocomm_ble"};
    bool quiet = true;
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        esp_log_level_set(tags[i], ESP_LOG_NONE);
        if (esp_log_level_get(tags[i]) != ESP_LOG_NONE) quiet = false;
    }
    return quiet;
}

static bool matches(security_t *security, uint32_t id)
{ return current == security && security->open && !security->revoked && security->session_id == id; }

void esp_openclaw_setup_security2_revoke(void)
{
    portENTER_CRITICAL(&lock);
    if (current) { current->peer = 0; current->revoked = true; }
    portEXIT_CRITICAL(&lock);
}

uint64_t esp_openclaw_setup_security2_peer(void)
{
    portENTER_CRITICAL(&lock);
    uint64_t peer = current && current->open && !current->revoked ? current->peer : 0;
    portEXIT_CRITICAL(&lock);
    return peer;
}

uint64_t esp_openclaw_setup_security2_peer_for_session(uint32_t id)
{
    portENTER_CRITICAL(&lock);
    uint64_t peer = current && matches(current, id) ? current->peer : 0;
    portEXIT_CRITICAL(&lock);
    return peer;
}

bool esp_openclaw_setup_security2_revoke_peer(uint64_t peer)
{
    portENTER_CRITICAL(&lock);
    bool matching = peer && current && current->peer == peer;
    if (matching) { current->peer = 0; current->revoked = true; }
    portEXIT_CRITICAL(&lock);
    return matching;
}

bool esp_openclaw_setup_security2_peer_is_current(uint64_t peer)
{ return peer != 0 && esp_openclaw_setup_security2_peer() == peer; }

static esp_err_t init(protocomm_security_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (current || protocomm_security2.ver != 2 || protocomm_security2.patch_ver != 1) return ESP_ERR_INVALID_STATE;
    if (!esp_openclaw_setup_security2_quiet_logs()) return ESP_ERR_INVALID_STATE;
    security_t *security = calloc(1, sizeof(*security));
    if (!security) return ESP_ERR_NO_MEM;
    esp_err_t err = protocomm_security2.init(&security->sdk);
    if (err != ESP_OK) { discard(security, sizeof(*security)); return err; }
    portENTER_CRITICAL(&lock); current = security; portEXIT_CRITICAL(&lock);
    *handle = security;
    return ESP_OK;
}

static esp_err_t cleanup(protocomm_security_handle_t handle)
{
    security_t *security = handle;
    if (!security) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    security->peer = 0; security->revoked = true;
    if (current == security) current = NULL;
    portEXIT_CRITICAL(&lock);
    esp_err_t err = protocomm_security2.cleanup(security->sdk);
    discard(security, sizeof(*security));
    return err;
}

static esp_err_t open_session(protocomm_security_handle_t handle, uint32_t id)
{
    security_t *security = handle;
    if (!security) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool admitted = current == security && !security->open;
    if (admitted) { security->open = true; security->revoked = false; security->session_id = id; security->peer = 0; }
    else if (current == security && security->session_id == id) { security->peer = 0; security->revoked = true; }
    portEXIT_CRITICAL(&lock);
    if (!admitted) return ESP_ERR_INVALID_STATE;
    /* SDK new_session closes its old ID unconditionally: admission MUST precede it. */
    esp_err_t err = protocomm_security2.new_transport_session(security->sdk, id);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&lock); security->open = false; security->revoked = true; portEXIT_CRITICAL(&lock);
    }
    return err;
}

static esp_err_t close_session(protocomm_security_handle_t handle, uint32_t id)
{
    security_t *security = handle;
    if (!security) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool admitted = current == security && security->open && security->session_id == id;
    if (admitted) { security->peer = 0; security->revoked = true; security->open = false; }
    portEXIT_CRITICAL(&lock);
    return admitted ? protocomm_security2.close_transport_session(security->sdk, id) : ESP_ERR_INVALID_STATE;
}

static esp_err_t request(protocomm_security_handle_t handle, const void *params, uint32_t id,
    const uint8_t *input, ssize_t size, uint8_t **output, ssize_t *output_size, void *context)
{
    security_t *security = handle;
    if (!security) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool admitted = matches(security, id) && security->peer == 0;
    if (matches(security, id) && security->peer != 0) { security->peer = 0; security->revoked = true; }
    portEXIT_CRITICAL(&lock);
    if (!admitted) return ESP_ERR_INVALID_STATE;
    /* No protobuf inspection or authentication inferred from handshake events. */
    return protocomm_security2.security_req_handler(security->sdk, params, id, input, size, output, output_size, context);
}

static esp_err_t crypt(protocomm_security_handle_t handle, uint32_t id,
    const uint8_t *input, ssize_t size, uint8_t **output, ssize_t *output_size, bool encrypting)
{
    security_t *security = handle;
    if (!security || !input || !output || !output_size || size <= (encrypting ? 0 : 16) ||
        size > ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX + (encrypting ? 0 : 16)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    bool admitted = matches(security, id) && (!encrypting || security->peer != 0);
    uint64_t peer = security->peer;
    portEXIT_CRITICAL(&lock);
    if (!admitted) return ESP_ERR_INVALID_STATE;
    uint8_t *bytes = NULL; ssize_t length = 0;
    esp_err_t err = encrypting ? protocomm_security2.encrypt(security->sdk, id, input, size, &bytes, &length) :
        protocomm_security2.decrypt(security->sdk, id, input, size, &bytes, &length);
    portENTER_CRITICAL(&lock);
    bool live = matches(security, id) && (!encrypting || security->peer == peer);
    if (err == ESP_OK && live && bytes && length == size + (encrypting ? 16 : -16)) {
        if (!encrypting && security->peer == 0) {
            if (last_peer == UINT64_MAX) live = false;
            else security->peer = ++last_peer;
        }
    } else live = false;
    if (!live) { security->peer = 0; security->revoked = true; }
    portEXIT_CRITICAL(&lock);
    if (!live) { discard(bytes, length > 0 ? (size_t)length : 0); return err != ESP_OK ? err : ESP_ERR_INVALID_STATE; }
    *output = bytes; *output_size = length;
    return ESP_OK;
}

static esp_err_t encrypt(protocomm_security_handle_t handle, uint32_t id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *output_size)
{ return crypt(handle, id, input, size, output, output_size, true); }
static esp_err_t decrypt(protocomm_security_handle_t handle, uint32_t id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *output_size)
{ return crypt(handle, id, input, size, output, output_size, false); }

const protocomm_security_t *esp_openclaw_setup_security2(void)
{
    static const protocomm_security_t boundary = {.ver = 2, .patch_ver = 1, .init = init, .cleanup = cleanup,
        .new_transport_session = open_session, .close_transport_session = close_session,
        .security_req_handler = request, .encrypt = encrypt, .decrypt = decrypt};
    return &boundary;
}

#else
const protocomm_security_t *esp_openclaw_setup_security2(void) { return NULL; }
bool esp_openclaw_setup_security2_quiet_logs(void) { return false; }
void esp_openclaw_setup_security2_revoke(void) {}
uint64_t esp_openclaw_setup_security2_peer(void) { return 0; }
uint64_t esp_openclaw_setup_security2_peer_for_session(uint32_t id) { (void)id; return 0; }
bool esp_openclaw_setup_security2_revoke_peer(uint64_t peer) { (void)peer; return false; }
bool esp_openclaw_setup_security2_peer_is_current(uint64_t peer) { (void)peer; return false; }
#endif
