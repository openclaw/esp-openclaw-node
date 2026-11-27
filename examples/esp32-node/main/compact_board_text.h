// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum { COMPACT_TEXT_COLUMNS = 25, COMPACT_TEXT_ROWS = 4, COMPACT_TEXT_MAX = 100 };
enum { COMPACT_LARGE_COLUMNS = 12, COMPACT_LARGE_ROWS = 2 };

static inline const char *compact_board_home_text(bool wifi_connected, bool gateway_connected)
{
    if (!wifi_connected) return "Connect USB at 115200\nType help to set up Wi-Fi";
    if (!gateway_connected) return "Wi-Fi connected\nType gateway over USB\nfor connection options";
    return "Ready for commands\nUse display.show to send\na message from OpenClaw";
}

// Keep accepted characters aligned with the small built-in font.
static inline bool compact_board_text_character(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || (c != '\0' && strchr(" .,-_:!?/+%()=", c) != NULL);
}

// Prefer word boundaries; split only words wider than the screen.
static inline bool compact_board_text_layout_width(const char *text, size_t rows, size_t columns,
                                                  char lines[][COMPACT_TEXT_COLUMNS + 1])
{
    if (text == NULL || rows == 0 || rows > COMPACT_TEXT_ROWS ||
        columns == 0 || columns > COMPACT_TEXT_COLUMNS) return false;
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
        if (rows > 1 && c != ' ' && column > 0 && i > 0 && text[i - 1] == ' ') {
            size_t word_length = 0;
            while (i + word_length <= COMPACT_TEXT_MAX && text[i + word_length] != '\0' &&
                   text[i + word_length] != ' ' && text[i + word_length] != '\n') {
                ++word_length;
            }
            if (word_length <= columns && column + word_length > columns) {
                while (column > 0 && lines[row][column - 1] == ' ') lines[row][--column] = '\0';
                ++row;
                column = 0;
            }
        }
        if (column == columns) {
            if (rows > 1 && c == ' ') continue;
            ++row;
            column = 0;
        }
        if (row >= rows) return false;
        lines[row][column++] = (char)c;
    }
    return true;
}

static inline bool compact_board_text_layout(const char *text, size_t rows,
                                            char lines[][COMPACT_TEXT_COLUMNS + 1])
{
    return compact_board_text_layout_width(text, rows, COMPACT_TEXT_COLUMNS, lines);
}
