#include "room_voice_draft.h"

#include <stdlib.h>
#include <string.h>

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
               intent.end != ROOM_VOICE_CANCEL) {
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
    free(audio);
    audio = NULL;
    discarding = true;
    discard_error = error;
    if (recording_owned) room_recording_cancel();
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

    if ((recording_owned || audio != NULL) &&
        (!context->valid || view.recipient_revision != context->recipient_revision ||
         view.owner_incarnation != context->owner_incarnation)) {
        discard("Destination changed. Record again.");
    }
    if (pending && begin) {
        discard(NULL);
        view = (room_voice_snapshot_t){.generation = generation,
            .recipient_revision = revision, .owner_incarnation = owner};
        strlcpy(view.recipient, context->recipient, sizeof(view.recipient));
        if (end == ROOM_VOICE_CANCEL) {
            /* A covered or canceled gesture never opens the microphone. */
        } else if (end == ROOM_VOICE_FINISH) {
            discard_error = "Released before recording started. Try again.";
        } else if (recording_owned || !context->valid || revision != context->recipient_revision ||
                   owner != context->owner_incarnation) {
            discard_error = recording_owned ? "Audio is finishing. Try again." : "Destination changed. Record again.";
        } else if (!context->available || room_media_request_recording(NULL, NULL) != ESP_OK) {
            discard_error = "Audio unavailable. Try again when ready.";
        } else {
            recording_owned = true;
            discarding = false;
            view.state = ROOM_VOICE_STARTING;
        }
    } else if (pending && generation == view.generation) {
        if (end == ROOM_VOICE_CANCEL) discard(NULL);
        else if (end == ROOM_VOICE_FINISH && recording_owned && !discarding) room_recording_stop();
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
                    view.state = ROOM_VOICE_DRAFT;
                } else {
                    set_error("Recording couldn't be kept. Try again.");
                }
            } else {
                set_error(recording_error(capture.error));
            }
        }
    }
    if (discarding && !recording_owned) {
        view.duration_ms = 0;
        view.detail[0] = '\0';
        if (discard_error != NULL) set_error(discard_error);
        else view.state = context->available ? ROOM_VOICE_READY : ROOM_VOICE_UNAVAILABLE;
        discarding = false;
    } else if (!recording_owned && audio == NULL && view.state != ROOM_VOICE_ERROR) {
        view.state = context->available ? ROOM_VOICE_READY : ROOM_VOICE_UNAVAILABLE;
    }
    if (!recording_owned && audio == NULL) {
        view.recipient_revision = context->recipient_revision;
        view.owner_incarnation = context->owner_incarnation;
    }
    portENTER_CRITICAL(&intent_mux);
    polling = recording_owned;
    portEXIT_CRITICAL(&intent_mux);
    bool changed = memcmp(&view, &published, sizeof(view)) != 0;
    published = view;
    *result = view;
    return changed;
}
