#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOM_HANDWRITING_POINTS_MAX 512
#define ROOM_HANDWRITING_MODEL_BYTES 106288

typedef struct {
    int16_t x, y;
    bool start;
} room_handwriting_point_t;

typedef enum {
    ROOM_HANDWRITING_EMPTY,
    ROOM_HANDWRITING_READY,
    ROOM_HANDWRITING_DENSE,
    ROOM_HANDWRITING_UNAVAILABLE,
} room_handwriting_result_t;

/* Upright, row-major white ink on black. Also used by the training recipe. */
void room_handwriting_normalize(const uint8_t source[784], uint8_t target[784]);
bool room_handwriting_logits(const uint8_t *model, size_t bytes,
    const uint8_t pixels[784], float logits[36]);
room_handwriting_result_t room_handwriting_raster(const room_handwriting_point_t *points,
    size_t count, uint8_t target[784]);
/* Case-insensitive English letters/digits, ranked candidates, never accepted text. */
room_handwriting_result_t room_handwriting_recognize(const uint8_t *model, size_t bytes,
    const room_handwriting_point_t *points, size_t count, char candidates[3]);
