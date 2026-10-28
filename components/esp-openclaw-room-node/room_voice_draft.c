#include "room_voice_draft.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "room_media.h"
#include "room_recording.h"

static portMUX_TYPE intent_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t generation;
    uint64_t recipient_revision;
    uint64_t owner_incarnation;
    room_voice_action_t end;
    bool pending, begin;
} intent;
static bool polling;
/* Supervisor-owned below this point. */
static room_voice_snapshot_t view, published;
static bool recording_owned, discarding;
static const char *discard_error;
static uint8_t *audio;
static size_t audio_bytes, audio_offset;
static esp_openclaw_transcription_handle_t transcription;
static bool draining;
/* Keep the bounded provider text off the supervisor's small task stack. */
static esp_openclaw_transcription_snapshot_t transcript;
static esp_openclaw_chat_handle_t chat;
static esp_openclaw_chat_snapshot_t chat_state;
static bool sent;
static bool chat_polling;
static bool queue_failed;

bool room_voice_draft_busy(void)
{
    return recording_owned || transcription != NULL;
}

bool room_voice_draft_connection_busy(void)
{
    return room_voice_draft_busy() || chat_polling || view.state == ROOM_VOICE_DRAFT;
}

void room_voice_draft_event(esp_openclaw_node_handle_t source, const char *event,
                           const char *payload, uint64_t revision, uint64_t owner)
{
    if (chat != NULL) esp_openclaw_chat_event(chat, source, event, payload);
    if (transcription != NULL && !draining && !discarding &&
        revision == view.recipient_revision && owner == view.owner_incarnation) {
        esp_openclaw_transcription_event(transcription, source, event, payload);
    }
}

void room_voice_draft_request(room_voice_action_t action, uint32_t generation, uint64_t revision, uint64_t owner)
{
    if (generation == 0) return;
    portENTER_CRITICAL(&intent_mux);
    if (action == ROOM_VOICE_BEGIN && generation > intent.generation) {
        intent.generation = generation;
        intent.recipient_revision = revision;
        intent.owner_incarnation = owner;
        intent.begin = true;
        intent.end = ROOM_VOICE_BEGIN;
        intent.pending = true;
    } else if (action != ROOM_VOICE_BEGIN && generation == intent.generation &&
               intent.end != ROOM_VOICE_CANCEL && (intent.end != ROOM_VOICE_HIDE || action == ROOM_VOICE_CANCEL)) {
        if (action == ROOM_VOICE_SEND) {
            intent.recipient_revision = revision;
            intent.owner_incarnation = owner;
        }
        intent.end = action;
        intent.pending = true;
    }
    portEXIT_CRITICAL(&intent_mux);
}

bool room_voice_draft_needs_poll(void)
{
    portENTER_CRITICAL(&intent_mux);
    bool needed = polling || intent.pending;
    portEXIT_CRITICAL(&intent_mux);
    return needed;
}

static void set_error(const char *message)
{
    view.state = ROOM_VOICE_ERROR;
    strlcpy(view.detail, message, sizeof(view.detail));
}

static void discard(const char *error)
{
    if (chat != NULL) esp_openclaw_chat_destroy(chat);
    chat = NULL;
    chat_polling = sent = queue_failed = false;
    view.can_send = false;
    free(audio);
    audio = NULL;
    audio_bytes = audio_offset = 0;
    view.text[0] = '\0';
    discarding = true;
    discard_error = error;
    if (recording_owned) room_recording_cancel();
    if (transcription != NULL) {
        view.state = ROOM_VOICE_CANCELING;
        draining = true;
        esp_openclaw_transcription_cancel(transcription);
    }
}

static void poll_chat(void)
{
    if (chat == NULL) return;
    esp_openclaw_chat_poll(chat);
    esp_openclaw_chat_copy(chat, &chat_state);
    view.can_send = !sent && chat_state.state == ESP_OPENCLAW_CHAT_READY;
    chat_polling = chat_state.state == ESP_OPENCLAW_CHAT_PREPARING ||
        chat_state.state == ESP_OPENCLAW_CHAT_SENDING || chat_state.state == ESP_OPENCLAW_CHAT_RUNNING;
    if (!sent) {
        if (view.state != ROOM_VOICE_DRAFT) return;
        if (chat_state.state == ESP_OPENCLAW_CHAT_FAILED)
            strlcpy(view.detail, "Open the conversation and record again.", sizeof(view.detail));
        else if (view.can_send) {
            strlcpy(view.detail, queue_failed ? "Could not queue the message. Try Send again." : "", sizeof(view.detail));
        } else strlcpy(view.detail, "Preparing conversation...", sizeof(view.detail));
        return;
    }
    view.state = ROOM_VOICE_SENDING;
    const char *detail = "Waiting for the Gateway to accept this message.";
    switch (chat_state.state) {
        case ESP_OPENCLAW_CHAT_RUNNING:
            view.state = ROOM_VOICE_ACCEPTED;
            detail = "Message accepted. Open Conversations for the reply.";
            break;
        case ESP_OPENCLAW_CHAT_COMPLETE:
            view.state = ROOM_VOICE_SENT;
            detail = "This turn finished. Open Conversations to read it.";
            break;
        case ESP_OPENCLAW_CHAT_HANDED_OFF:
            view.state = ROOM_VOICE_SENT;
            detail = "Message accepted. Work continues in the conversation.";
            break;
        case ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN:
            view.state = ROOM_VOICE_SEND_UNKNOWN;
            detail = "Check the conversation before sending again.";
            break;
        case ESP_OPENCLAW_CHAT_FAILED:
        case ESP_OPENCLAW_CHAT_ABORTED:
            view.state = ROOM_VOICE_SEND_FAILED;
            detail = "The turn stopped. Check the conversation for details.";
            break;
        default: break;
    }
    strlcpy(view.detail, detail, sizeof(view.detail));
}

static void send_draft(void)
{
    if (chat == NULL || !view.can_send || view.state != ROOM_VOICE_DRAFT || sent) return;
    uint8_t bytes[16];
    char id[33];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) snprintf(id + i * 2, 3, "%02x", bytes[i]);
    if (esp_openclaw_chat_send(chat, view.text, id) == ESP_OK) {
        sent = true;
        view.can_send = false;
        poll_chat();
    } else {
        queue_failed = true;
        strlcpy(view.detail, "Could not queue the message. Try Send again.", sizeof(view.detail));
    }
}

static void transcription_error(const char *message)
{
    view.state = ROOM_VOICE_TRANSCRIPTION_ERROR;
    view.text[0] = '\0';
    strlcpy(view.detail, message, sizeof(view.detail));
}

static void poll_transcription(void)
{
    if (transcription == NULL) return;
    esp_openclaw_transcription_poll(transcription);
    if (!draining) {
        esp_openclaw_transcription_copy(transcription, &transcript);
        if (transcript.state == ESP_OPENCLAW_TRANSCRIPTION_READY) {
            esp_err_t err;
            if (audio_offset < audio_bytes) {
                size_t length = audio_bytes - audio_offset;
                if (length > ESP_OPENCLAW_TRANSCRIPTION_CHUNK_BYTES) length = ESP_OPENCLAW_TRANSCRIPTION_CHUNK_BYTES;
                err = esp_openclaw_transcription_append(transcription, audio + audio_offset, length);
                if (err == ESP_OK) audio_offset += length;
            } else {
                err = esp_openclaw_transcription_finish(transcription);
            }
            if (err != ESP_OK) {
                transcription_error("Couldn't transcribe. Record again.");
                draining = true;
            }
        } else if (transcript.state == ESP_OPENCLAW_TRANSCRIPTION_COMPLETE) {
            if (transcript.text[0] == '\0') transcription_error("No speech heard. Record again.");
            else {
                strlcpy(view.text, transcript.text, sizeof(view.text));
                view.state = ROOM_VOICE_DRAFT;
            }
            draining = true;
        } else if (transcript.state == ESP_OPENCLAW_TRANSCRIPTION_FAILED ||
                   transcript.state == ESP_OPENCLAW_TRANSCRIPTION_CANCELED) {
            transcription_error(transcript.error == ESP_OPENCLAW_TRANSCRIPTION_ERROR_TIMEOUT
                ? "Transcription timed out. Record again."
                : transcript.error == ESP_OPENCLAW_TRANSCRIPTION_ERROR_CONNECTION
                ? "Connection lost. Reconnect and record again."
                : "Couldn't transcribe. Record again.");
            draining = true;
        }
    }
    if (draining) {
        free(audio);
        audio = NULL;
        audio_bytes = audio_offset = 0;
        if (esp_openclaw_transcription_destroy(transcription) == ESP_OK) {
            transcription = NULL;
            draining = false;
        }
    }
}

static const char *recording_error(room_recording_error_t error)
{
    switch (error) {
        case ROOM_RECORDING_ERROR_MEMORY: return "Not enough memory. Try again.";
        case ROOM_RECORDING_ERROR_CODEC: return "Audio couldn't start. Try again.";
        case ROOM_RECORDING_ERROR_OVERFLOW: return "Recording fell behind. Try again.";
        case ROOM_RECORDING_ERROR_SOURCE: return "Microphone input stopped. Try again.";
        default: return "Nothing recorded. Hold a little longer.";
    }
}

bool room_voice_draft_poll(const room_voice_context_t *context, room_voice_snapshot_t *result)
{
    portENTER_CRITICAL(&intent_mux);
    bool pending = intent.pending, begin = intent.begin;
    uint32_t generation = intent.generation;
    uint64_t revision = intent.recipient_revision;
    uint64_t owner = intent.owner_incarnation;
    room_voice_action_t end = intent.end;
    intent.pending = false;
    intent.begin = false;
    portEXIT_CRITICAL(&intent_mux);

    if (!sent && !discarding && (room_voice_draft_busy() || view.state == ROOM_VOICE_DRAFT) &&
        (!context->valid || view.recipient_revision != context->recipient_revision ||
         view.owner_incarnation != context->owner_incarnation)) {
        discard("Destination changed. Record again.");
    }
    if (pending && begin) {
        discard(NULL);
        view = (room_voice_snapshot_t){.generation = generation,
            .recipient_revision = revision, .owner_incarnation = owner};
        strlcpy(view.recipient, context->recipient, sizeof(view.recipient));
        if (transcription != NULL) view.state = ROOM_VOICE_CANCELING;
        if (end == ROOM_VOICE_CANCEL || end == ROOM_VOICE_HIDE) {
            /* A covered or canceled gesture never opens the microphone. */
        } else if (end == ROOM_VOICE_FINISH) {
            discard_error = "Released before recording started. Try again.";
        } else if (room_voice_draft_busy() || !context->valid || revision != context->recipient_revision ||
                   owner != context->owner_incarnation) {
            discard_error = room_voice_draft_busy() ? "Audio is finishing. Try again." : "Destination changed. Record again.";
        } else if (!context->available || room_media_request_recording(NULL, NULL) != ESP_OK) {
            discard_error = "Audio unavailable. Try again when ready.";
        } else {
            recording_owned = true;
            discarding = false;
            view.state = ROOM_VOICE_STARTING;
            strlcpy(view.agent_id, context->agent_id, sizeof(view.agent_id));
            if (context->send_enabled && context->session_key[0] != '\0' && context->agent_id[0] != '\0' &&
                esp_openclaw_chat_prepare(context->operator_node, context->session_key,
                    context->agent_id, &chat) == ESP_OK) {
                chat_polling = true;
                strlcpy(view.detail, "Preparing conversation...", sizeof(view.detail));
            } else strlcpy(view.detail, context->agent_id[0] == '\0' ? "Choose an agent before recording to send."
                : !context->send_enabled ? "Enable voice actions in firmware to send."
                : "Open the conversation and record again.", sizeof(view.detail));
        }
    } else if (pending && generation == view.generation) {
        if (end == ROOM_VOICE_CANCEL || (end == ROOM_VOICE_HIDE && !sent)) discard(NULL);
        else if (end == ROOM_VOICE_FINISH && recording_owned && !discarding) room_recording_stop();
        else if (end == ROOM_VOICE_SEND && revision == view.recipient_revision &&
                 owner == view.owner_incarnation && context->valid) send_draft();
    }
    if (recording_owned) {
        /* Completion publishes terminal state before clearing active. Read
         * active first so an inactive observation always has a final snapshot.
         * A worker finishing after this read is picked up on the next poll. */
        bool active = room_recording_active();
        room_recording_snapshot_t capture;
        room_recording_snapshot(&capture);
        view.duration_ms = (uint32_t)(capture.audio_bytes / 8);
        if (active) {
            view.state = discarding || capture.state == ROOM_RECORDING_STOPPING ? ROOM_VOICE_FINISHING
                : capture.state == ROOM_RECORDING_ACTIVE ? ROOM_VOICE_RECORDING : ROOM_VOICE_STARTING;
        } else {
            recording_owned = false;
            if (discarding) {
                /* Cancel already discarded the backend result. */
            } else if (capture.state == ROOM_RECORDING_READY) {
                size_t bytes = 0;
                if (room_recording_take(&audio, &bytes) == ESP_OK) {
                    view.duration_ms = (uint32_t)(bytes / 8);
                    audio_bytes = bytes;
                    audio_offset = 0;
                    if (esp_openclaw_transcription_begin(context->operator_node, &transcription) == ESP_OK) {
                        view.state = ROOM_VOICE_TRANSCRIBING;
                    } else {
                        free(audio);
                        audio = NULL;
                        audio_bytes = 0;
                        transcription_error("Couldn't start transcription. Record again.");
                    }
                } else {
                    set_error("Recording couldn't be kept. Try again.");
                }
            } else {
                set_error(recording_error(capture.error));
            }
        }
    }
    poll_transcription();
    poll_chat();
    if (discarding && !room_voice_draft_busy()) {
        view.duration_ms = 0;
        view.detail[0] = '\0';
        if (discard_error != NULL) set_error(discard_error);
        else view.state = context->available ? ROOM_VOICE_READY : ROOM_VOICE_UNAVAILABLE;
        discarding = false;
    } else if (!sent && !room_voice_draft_busy() && view.state != ROOM_VOICE_ERROR &&
               view.state != ROOM_VOICE_TRANSCRIPTION_ERROR && view.state != ROOM_VOICE_DRAFT) {
        view.state = context->available ? ROOM_VOICE_READY : ROOM_VOICE_UNAVAILABLE;
    }
    if (!sent && !room_voice_draft_busy() && view.state != ROOM_VOICE_DRAFT) {
        view.recipient_revision = context->recipient_revision;
        view.owner_incarnation = context->owner_incarnation;
    }
    portENTER_CRITICAL(&intent_mux);
    polling = room_voice_draft_busy() || chat_polling;
    portEXIT_CRITICAL(&intent_mux);
    bool changed = memcmp(&view, &published, sizeof(view)) != 0;
    published = view;
    *result = view;
    return changed;
}
