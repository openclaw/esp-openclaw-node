#pragma once

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_openclaw_node.h"
#include "esp_webrtc.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "room_ui_controller.h"
#include "room_media.h"
#include "room_recording.h"
#include "esp_openclaw_node_wifi.h"

/* Single-threaded schedule points, not a model of the controller. */
typedef struct {
    unsigned opens, starts, closes, media_begins, media_ends, ambient_restores;
    unsigned config_requests, create_requests, close_requests;
    char create_key[257];
    unsigned selection_completions, ui_refreshes;
    room_ui_talk_target_t ui_target;
    esp_err_t selection_result;
    unsigned speech_requests;
    char speech_params[1536];
    unsigned scope_requests, token_connects;
    unsigned agent_requests, conversation_requests, device_requests;
    bool devices_wanted;
    unsigned devices_generation;
    esp_openclaw_devices_state_t devices_state;
    bool fail_device_submit;
    bool resume_wanted, resume_fallback, expire_queue_wait;
    unsigned resume_generation, resume_attempts, queue_wait;
    esp_err_t resume_result;
    char resumed_agent[65];
    char reader_method[32], reader_params[1024];
    room_agents_state_t agents_state;
    bool fail_agent_submit;
    int64_t now_us;
    char scope_method[40], scope_params[256], token_value[80], token_uri[256];
    esp_openclaw_node_handle_t token_node;
    bool fail_scope_submit;
    unsigned ice_callbacks, signaling_connected, signaling_closed, peer_disconnects;
    unsigned critical_depth, callback_depth;
    bool media_owned, ambient, inside_start, media_busy;
    unsigned recording_starts, recording_stops, recording_cancels;
    unsigned transcription_creates, transcription_appends, transcription_closes, transcription_bytes;
    bool recording_active;
    bool finish_after_recording_snapshot;
    room_recording_snapshot_t recording;
    room_voice_snapshot_t voice;
    bool reject_task_creation;
    unsigned operator_connect_requests, node_connect_requests;
    room_ui_state_t ui;
    room_ui_facts_t home;
    const char *board_model_identifier;
    esp_openclaw_node_wifi_status_t wifi;
    esp_err_t node_connect_result, operator_connect_result;
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
    const char *operator_uri;
    const char *create_voice;
    bool fail_log_policy, capture_diagnostics;
    esp_log_level_t global_log_level, sdk_log_level;
    unsigned log_set_calls;
    unsigned audio_snapshots;
    char diagnostics[16384];
    bool console_snapshots;
    room_media_tone_snapshot_t console_tone;
    const char *console_tone_state_name, *console_tone_error_name;
    unsigned console_tone_error_name_calls;
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
void host_reply_speech(bool ok);
void host_reply_scope(bool ok, const char *payload);
void host_reply_transcription(bool ok, const char *payload);
void host_reply_agents(bool ok, const char *payload);
void host_reply_devices(bool ok, const char *payload);
void host_fire_timeout(TimerHandle_t timer);
void host_fire_timeout_generation(TimerHandle_t timer, uint32_t generation);
void host_command(esp_openclaw_node_handle_t node, const char *name);
esp_err_t host_command_params(esp_openclaw_node_handle_t node, const char *name, const char *params);
void host_fire_operator_timer(esp_timer_handle_t timer);
void host_fire_directory_timer(void);
void host_fire_devices_timer(void);
bool host_timer_active(TimerHandle_t timer);
unsigned host_node_scope_count(esp_openclaw_node_handle_t node);
bool host_node_has_scope(esp_openclaw_node_handle_t node, const char *scope);

/* strlcpy is not in POSIX C (including Darwin with strict feature macros).
 * The room diagnostics snapshot needs this declaration. */
size_t strlcpy(char *destination, const char *source, size_t capacity);
