// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum { COMPACT_TEXT_COLUMNS = 25, COMPACT_TEXT_ROWS = 4, COMPACT_TEXT_MAX = 100 };

// Keep accepted characters aligned with the small built-in font.
static inline bool compact_board_text_character(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || (c != '\0' && strchr(" .,-_:!?/+%()=", c) != NULL);
}

// Wrap before the next character, so a newline after a full row does not skip a row.
static inline bool compact_board_text_layout(const char *text, size_t rows,
                                            char lines[][COMPACT_TEXT_COLUMNS + 1])
{
    if (text == NULL || rows == 0 || rows > COMPACT_TEXT_ROWS) return false;
    memset(lines, 0, rows * (COMPACT_TEXT_COLUMNS + 1));
    size_t row = 0, column = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (i >= COMPACT_TEXT_MAX) return false;
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' && rows > 1) {
            if (++row >= rows) return false;
            column = 0;
            continue;
        }
        if (!compact_board_text_character(c)) return false;
        if (column == COMPACT_TEXT_COLUMNS) {
            ++row;
            column = 0;
        }
        if (row >= rows) return false;
        lines[row][column++] = (char)c;
    }
    return true;
}
