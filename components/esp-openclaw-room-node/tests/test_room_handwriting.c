#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "room_handwriting.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    uint8_t *model = malloc(106288); assert(model);
    assert(fread(model, 1, 106288, file) == 106288); assert(fgetc(file) == EOF); fclose(file);
    uint8_t pixels[784], raw[784] = {0}; float logits[36];
    assert(room_handwriting_raster(NULL, 0, pixels) == ROOM_HANDWRITING_EMPTY);
    room_handwriting_point_t tap[] = {{10,10,true}, {10,10,false}};
    assert(room_handwriting_raster(tap, 2, pixels) == ROOM_HANDWRITING_EMPTY);
    room_handwriting_point_t small[] = {{10,10,true}, {12,12,false}};
    assert(room_handwriting_raster(small, 2, pixels) == ROOM_HANDWRITING_EMPTY);
    room_handwriting_point_t l[] = {{0,0,true}, {0,100,false}, {60,100,false}};
    assert(room_handwriting_raster(l, 3, pixels) == ROOM_HANDWRITING_READY);
    assert(room_handwriting_logits(model, 106288, pixels, logits));
    unsigned best = 0;
    for (unsigned i = 1; i < 36; ++i) if (logits[i] > logits[best]) best = i;
    assert(best == 21); /* Upright L, not a mirrored or transposed fixture. */
    char candidates[3];
    assert(room_handwriting_recognize(model, 106288, l, 3, candidates) == ROOM_HANDWRITING_READY);
    assert(candidates[0] == 'l' && candidates[1] == '1' && candidates[2] == 'i');
    const room_handwriting_point_t shapes[][9] = {
        {{0,100,true},{0,0,false},{60,0,false},{0,45,true},{45,45,false}},
        {{0,100,true},{0,0,false},{50,0,false},{65,15,false},{65,35,false},{50,50,false},{0,50,false},{30,50,true},{65,100,false}},
        {{0,20,true},{15,0,false},{50,0,false},{70,20,false},{65,40,false},{0,100,false},{70,100,false}},
        {{50,0,true},{0,65,false},{70,65,false},{50,0,true},{50,100,false}},
        {{0,0,true},{70,0,false},{20,100,false}}
    };
    const unsigned counts[] = {5,9,7,5,3}, expected[] = {15,27,2,4,7};
    for (unsigned fixture = 0; fixture < 5; ++fixture) {
        assert(room_handwriting_raster(shapes[fixture], counts[fixture], pixels) == ROOM_HANDWRITING_READY);
        assert(room_handwriting_logits(model, 106288, pixels, logits));
        best = 0;
        for (unsigned i = 1; i < 36; ++i) if (logits[i] > logits[best]) best = i;
        assert(best == expected[fixture]);
    }
    assert(room_handwriting_raster(l, 3, pixels) == ROOM_HANDWRITING_READY);
    unsigned left = 0, right = 0, top = 0, bottom = 0;
    for (unsigned y = 0; y < 28; ++y) for (unsigned x = 0; x < 28; ++x) {
        if (x < 14) left += pixels[y*28+x]; else right += pixels[y*28+x];
        if (y < 14) top += pixels[y*28+x]; else bottom += pixels[y*28+x];
    }
    assert(left > right && bottom > top);
    room_handwriting_point_t separate[] = {{0,0,true}, {20,0,false}, {80,100,true}, {100,100,false}};
    assert(room_handwriting_raster(separate, 4, pixels) == ROOM_HANDWRITING_READY);
    for (unsigned y = 10; y <= 17; ++y) for (unsigned x = 0; x < 28; ++x) assert(pixels[y*28+x] == 0);
    room_handwriting_point_t dense[68];
    for (unsigned i = 0; i < 34; ++i) {
        dense[i*2] = (room_handwriting_point_t){0,(int16_t)(i*3),true};
        dense[i*2+1] = (room_handwriting_point_t){100,(int16_t)(i*3),false};
    }
    assert(room_handwriting_raster(dense, 68, pixels) == ROOM_HANDWRITING_DENSE);
    for (unsigned i = 0; i < 784; ++i) {
        memset(raw, 0, sizeof(raw)); raw[i] = 255;
        room_handwriting_normalize(raw, pixels);
        assert(room_handwriting_logits(model, 106288, pixels, logits));
    }
    assert(!room_handwriting_logits(model, 106287, pixels, logits));
    model[0] ^= 1; assert(!room_handwriting_logits(model, 106288, pixels, logits));
    room_handwriting_point_t too_many[ROOM_HANDWRITING_POINTS_MAX + 1] = {0};
    assert(room_handwriting_raster(too_many, ROOM_HANDWRITING_POINTS_MAX + 1, pixels) == ROOM_HANDWRITING_UNAVAILABLE);
    free(model);
    puts("handwriting kernel: upright/aspect, pen-up, blank/tap, dense flood, all isolated pixels and model bounds passed");
}
