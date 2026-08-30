#pragma once

#include <stdbool.h>
#include <stddef.h>

bool room_file_normalize_public_path(
    const char *root,
    const char *path,
    char *out,
    size_t out_size);

bool room_file_inspect_base64(const char *value, size_t *decoded_len);

/** Preflight calls validate and hash content but never perform filesystem mutations. */
bool room_file_mutation_allowed(bool preflight_only);

/** First char after root/ for the mkdir walk, or NULL when parent is the root. */
char *room_file_parent_walk_start(char *parent, const char *root);

