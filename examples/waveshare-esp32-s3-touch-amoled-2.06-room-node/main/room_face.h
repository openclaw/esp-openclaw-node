#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "lvgl.h"

/** Talk-driven animation states; moods restyle them without changing state. */
typedef enum {
    ROOM_FACE_HIDDEN = 0,
    ROOM_FACE_LISTENING,
    ROOM_FACE_THINKING,
    ROOM_FACE_SPEAKING,
    /* Idle pop-up (agent face.set outside a call); expires from the face tick. */
    ROOM_FACE_HINT,
} room_face_state_t;

/** Short authored keyframe gestures layered over the tween rig. */
typedef enum {
    ROOM_FACE_GESTURE_SURPRISE = 0,
    ROOM_FACE_GESTURE_YAWN,
    ROOM_FACE_GESTURE_NOD,
    ROOM_FACE_GESTURE_SHAKE,
} room_face_gesture_t;

/** Create the face widgets on `parent`. Display lock must be held. */
esp_err_t room_face_create(lv_obj_t *parent);

/** Show the face in `state` / hide it. Display lock must be held. */
void room_face_show(room_face_state_t state);
/** Show the hint face until `until_us`; the tick expires it. Lock held. */
void room_face_show_hint(int64_t until_us);
void room_face_hide(void);
bool room_face_is_visible(void);
/** Drop any held mood back to neutral. Display lock must be held. */
void room_face_reset_mood(void);

/** Play a gesture; takes the display lock internally, callable from any task. */
void room_face_play_gesture(room_face_gesture_t gesture);

/** Latest playback loudness 0..255; called from the audio render task, lock-free. */
void room_face_set_speech_level(uint8_t level);

/** Registers the face.set and face.gesture node commands. */
esp_err_t room_face_register_node_commands(esp_openclaw_node_handle_t node);
