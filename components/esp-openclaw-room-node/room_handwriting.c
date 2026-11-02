#include "room_handwriting.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void room_handwriting_normalize(const uint8_t source[784], uint8_t target[784])
{
    unsigned x0 = 28, y0 = 28, x1 = 0, y1 = 0;
    memset(target, 0, 784);
    for (unsigned y = 0; y < 28; ++y) for (unsigned x = 0; x < 28; ++x) {
        if (source[y * 28 + x] <= 32) continue;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x0 == 28) return;
    unsigned width = x1 - x0 + 1, height = y1 - y0 + 1;
    unsigned largest = width > height ? width : height;
    unsigned nw = (width * 20 + largest / 2) / largest;
    unsigned nh = (height * 20 + largest / 2) / largest;
    if (!nw) nw = 1;
    if (!nh) nh = 1;
    unsigned ox = (28 - nw) / 2, oy = (28 - nh) / 2;
    uint8_t centered[784] = {0};
    for (unsigned y = 0; y < nh; ++y) for (unsigned x = 0; x < nw; ++x) {
        float sx = ((float)x + 0.5f) * width / nw - 0.5f;
        float sy = ((float)y + 0.5f) * height / nh - 0.5f;
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        unsigned ax = (unsigned)sx, ay = (unsigned)sy;
        unsigned bx = ax + 1 < width ? ax + 1 : ax, by = ay + 1 < height ? ay + 1 : ay;
        float fx = sx - ax, fy = sy - ay;
        float top = source[(y0 + ay) * 28 + x0 + ax] * (1 - fx) + source[(y0 + ay) * 28 + x0 + bx] * fx;
        float bottom = source[(y0 + by) * 28 + x0 + ax] * (1 - fx) + source[(y0 + by) * 28 + x0 + bx] * fx;
        centered[(oy + y) * 28 + ox + x] = (uint8_t)(top * (1 - fy) + bottom * fy + 0.5f);
    }
    unsigned mass = 0, mx = 0, my = 0;
    for (unsigned y = 0; y < 28; ++y) for (unsigned x = 0; x < 28; ++x) {
        unsigned value = centered[y * 28 + x];
        mass += value;
        mx += x * value;
        my += y * value;
    }
    if (!mass) return;
    int dx = (int)floorf(13.5f - (float)mx / mass + 0.5f);
    int dy = (int)floorf(13.5f - (float)my / mass + 0.5f);
    if (dx < -(int)ox) dx = -(int)ox;
    if (dx > (int)(28 - ox - nw)) dx = (int)(28 - ox - nw);
    if (dy < -(int)oy) dy = -(int)oy;
    if (dy > (int)(28 - oy - nh)) dy = (int)(28 - oy - nh);
    for (unsigned y = oy; y < oy + nh; ++y) for (unsigned x = ox; x < ox + nw; ++x)
        target[(y + dy) * 28 + x + dx] = centered[y * 28 + x];
}

static float little_float(const uint8_t *bytes)
{
    uint32_t bits = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

bool room_handwriting_logits(const uint8_t *data, size_t bytes,
    const uint8_t pixels[784], float logits[36])
{
    if (bytes != ROOM_HANDWRITING_MODEL_BYTES || memcmp(data, "OCHW01\0\0", 8) != 0 ||
        data[8] != 16 || data[9] != 3 || data[10] != 128 || data[11] != 0 || data[12] != 36 || data[13] != 0)
        return false;
    const int8_t *first = (const int8_t *)(data + 16), *second = (const int8_t *)(data + 101392);
    const uint8_t *first_scale = data + 100368, *first_bias = data + 100880;
    const uint8_t *second_scale = data + 106000, *second_bias = data + 106144;
    float hidden[128];
    for (unsigned row = 0; row < 128; ++row) {
        int32_t sum = 0;
        for (unsigned column = 0; column < 784; ++column)
            sum += (int32_t)first[row * 784 + column] * pixels[column];
        float value = (float)sum * (little_float(first_scale + row * 4) / 255.0f) + little_float(first_bias + row * 4);
        hidden[row] = value > 0 ? value : 0;
    }
    for (unsigned row = 0; row < 36; ++row) {
        float sum = 0;
        for (unsigned column = 0; column < 128; ++column)
            sum += (float)second[row * 128 + column] * hidden[column];
        logits[row] = sum * little_float(second_scale + row * 4) + little_float(second_bias + row * 4);
    }
    return true;
}

static void dot(uint8_t pixels[784], int x, int y)
{
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        if (x + dx >= 0 && x + dx < 28 && y + dy >= 0 && y + dy < 28)
            pixels[(y + dy) * 28 + x + dx] = 255;
}

static void line(uint8_t pixels[784], int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, error = dx + dy;
    for (;;) {
        dot(pixels, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

room_handwriting_result_t room_handwriting_raster(const room_handwriting_point_t *points,
    size_t count, uint8_t target[784])
{
    memset(target, 0, 784);
    if (count > ROOM_HANDWRITING_POINTS_MAX) return ROOM_HANDWRITING_UNAVAILABLE;
    if (count < 2) return ROOM_HANDWRITING_EMPTY;
    int x0 = points[0].x, x1 = x0, y0 = points[0].y, y1 = y0;
    unsigned movement = 0;
    for (size_t i = 0; i < count; ++i) {
        if (points[i].x < x0) x0 = points[i].x;
        if (points[i].x > x1) x1 = points[i].x;
        if (points[i].y < y0) y0 = points[i].y;
        if (points[i].y > y1) y1 = points[i].y;
        if (i && !points[i].start)
            movement += abs(points[i].x - points[i - 1].x) + abs(points[i].y - points[i - 1].y);
    }
    int width = x1 - x0, height = y1 - y0, largest = width > height ? width : height;
    if (largest < 8 || movement < 8) return ROOM_HANDWRITING_EMPTY;
    uint8_t raw[784] = {0};
    int previous_x = 0, previous_y = 0;
    for (size_t i = 0; i < count; ++i) {
        int x = 2 + ((points[i].x - x0) * 23 + largest / 2) / largest + (23 - width * 23 / largest) / 2;
        int y = 2 + ((points[i].y - y0) * 23 + largest / 2) / largest + (23 - height * 23 / largest) / 2;
        if (i == 0 || points[i].start) dot(raw, x, y);
        else line(raw, previous_x, previous_y, x, y);
        previous_x = x;
        previous_y = y;
    }
    room_handwriting_normalize(raw, target);
    unsigned ink = 0;
    for (unsigned i = 0; i < 784; ++i) ink += target[i] > 32;
    /* Reject a flood, not arbitrary unknown symbols. Candidates still need review. */
    return ink > 320 ? ROOM_HANDWRITING_DENSE : ROOM_HANDWRITING_READY;
}

room_handwriting_result_t room_handwriting_recognize(const uint8_t *model, size_t bytes,
    const room_handwriting_point_t *points, size_t count, char candidates[3])
{
    uint8_t pixels[784];
    room_handwriting_result_t result = room_handwriting_raster(points, count, pixels);
    if (result != ROOM_HANDWRITING_READY) return result;
    float logits[36];
    if (!room_handwriting_logits(model, bytes, pixels, logits))
        return ROOM_HANDWRITING_UNAVAILABLE;
    for (unsigned rank = 0; rank < 3; ++rank) {
        unsigned best = 0;
        for (unsigned i = 1; i < 36; ++i) if (logits[i] > logits[best]) best = i;
        candidates[rank] = "0123456789abcdefghijklmnopqrstuvwxyz"[best];
        logits[best] = -INFINITY;
    }
    return ROOM_HANDWRITING_READY;
}
