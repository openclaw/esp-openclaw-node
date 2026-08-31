#pragma once

#include "sdkconfig.h"
#include "esp_openclaw_node.h"
#include "esp_webrtc.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "room_ui_controller.h"

/* Single-threaded schedule points, not a model of the controller. */
typedef struct {
    unsigned opens, starts, closes, media_begins, media_ends, ambient_restores;
    unsigned config_requests, create_requests, close_requests;
    unsigned ice_callbacks, signaling_connected, signaling_closed, peer_disconnects;
    unsigned critical_depth, callback_depth;
    bool media_owned, ambient, inside_start;
    room_ui_state_t ui;
    char close_voice[129], close_key[257];
    esp_openclaw_node_handle_t close_node;
    void (*at_media_begin)(void);
    void (*inside_webrtc_start)(void);
    void (*before_signaling_start)(void);
    void (*inside_close)(void);
    void (*inside_ambient_restore)(void);
    void (*inside_media_end)(void);
    bool reuse_rtc, last_started;
    bool fail_config_submit, fail_create_submit, fail_open, fail_provider, fail_start, fail_timer;
    const char *node_uri;
    const char *create_voice;
} room_host_observations_t;
extern room_host_observations_t host;

void host_require(bool condition, const char *message);
void host_run_task(const char *name);
void host_release_resources(void);
void host_emit_node(esp_openclaw_node_handle_t node, esp_openclaw_node_event_t event);
void host_emit_gateway(esp_openclaw_node_handle_t node, const char *event, const char *payload);
void host_emit_peer(esp_webrtc_handle_t session, esp_webrtc_event_type_t event);
void host_reply_config(void);
void host_reply_create(bool ok);
void host_fire_timeout(TimerHandle_t timer);
void host_command(esp_openclaw_node_handle_t node, const char *name);
void host_fire_operator_timer(esp_timer_handle_t timer);
bool host_timer_active(TimerHandle_t timer);

/* strlcpy is not in POSIX C (including Darwin with strict feature macros).
 * Only the unlinked room diagnostics path needs this declaration. */
size_t strlcpy(char *destination, const char *source, size_t capacity);
