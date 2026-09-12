#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "room_file_validation.h"

static char *zero_base64_for_size(size_t decoded_size)
{
    size_t encoded_size = ((decoded_size + 2U) / 3U) * 4U;
    char *encoded = malloc(encoded_size + 1U);
    assert(encoded != NULL);
    memset(encoded, 'A', encoded_size);
    if (decoded_size % 3U == 1U) {
        encoded[encoded_size - 2U] = '=';
        encoded[encoded_size - 1U] = '=';
    } else if (decoded_size % 3U == 2U) {
        encoded[encoded_size - 1U] = '=';
    }
    encoded[encoded_size] = '\0';
    return encoded;
}

int main(void)
{
    size_t decoded = 0;
    assert(room_file_inspect_base64("", &decoded) && decoded == 0);
    assert(room_file_inspect_base64("//8=", &decoded) && decoded == 2);
    assert(room_file_inspect_base64("__8", &decoded) && decoded == 2);
    assert(!room_file_inspect_base64("AB", &decoded));
    assert(!room_file_inspect_base64("AA=A", &decoded));

    char *at_limit = zero_base64_for_size(1024U * 1024U);
    char *above_limit = zero_base64_for_size((1024U * 1024U) + 1U);
    assert(room_file_inspect_base64(at_limit, &decoded) && decoded == 1024U * 1024U);
    assert(room_file_inspect_base64(above_limit, &decoded) && decoded == (1024U * 1024U) + 1U);
    free(at_limit);
    free(above_limit);

    char path[64];
    assert(room_file_normalize_public_path("/sd", "/sd/", path, sizeof(path)));
    assert(strcmp(path, "/sd") == 0);
    assert(room_file_normalize_public_path("/sd", "/sd/a.txt/", path, sizeof(path)));
    assert(strcmp(path, "/sd/a.txt") == 0);
    assert(!room_file_normalize_public_path("/sd", "/sd/../secret", path, sizeof(path)));
    assert(!room_file_normalize_public_path("/sd", "/sd//alias", path, sizeof(path)));
    assert(!room_file_normalize_public_path("/sd", "sd/relative", path, sizeof(path)));
    assert(!room_file_normalize_public_path("/sd", "/sdcard/alias", path, sizeof(path)));

    assert(!room_file_mutation_allowed(true));
    assert(room_file_mutation_allowed(false));

    char parent[16];
    memset(parent, 'X', sizeof(parent));
    memcpy(parent, "/sd", 4);
    assert(room_file_parent_walk_start(parent, "/sd") == NULL);
    assert(room_file_parent_walk_start(NULL, "/sd") == NULL);
    assert(room_file_parent_walk_start(parent, NULL) == NULL);

    memset(parent, 'X', sizeof(parent));
    memcpy(parent, "/sd/a/b", 8);
    char *walk = room_file_parent_walk_start(parent, "/sd");
    assert(walk == parent + 4);
    assert(*walk == 'a');

    puts("room file validation tests passed");
    return 0;
}

