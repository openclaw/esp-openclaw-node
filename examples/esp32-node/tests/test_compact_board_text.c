// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <stdio.h>
#include "compact_board_text.h"

int main(void)
{
    char lines[COMPACT_TEXT_ROWS][COMPACT_TEXT_COLUMNS + 1];
    assert(compact_board_text_layout("", 1, lines));
    assert(lines[0][0] == '\0');
    assert(compact_board_text_layout("Hello, OpenClaw!", 1, lines));
    assert(strcmp(lines[0], "Hello, OpenClaw!") == 0);
    assert(compact_board_text_layout(" .,-_:!?/+%()=", 1, lines));
    assert(!compact_board_text_layout("hello\nworld", 1, lines));
    assert(!compact_board_text_layout("hello\tworld", 4, lines));
    assert(!compact_board_text_layout("hello\rworld", 4, lines));
    assert(!compact_board_text_layout("caf\xc3\xa9", 4, lines));
    assert(!compact_board_text_layout("unsupported @", 4, lines));
    assert(!compact_board_text_layout(NULL, 4, lines));
    assert(!compact_board_text_layout("hello", 0, lines));
    assert(!compact_board_text_layout("hello", 5, lines));

    char full[COMPACT_TEXT_MAX + 2];
    memset(full, 'A', sizeof(full));
    full[COMPACT_TEXT_COLUMNS] = '\0';
    assert(compact_board_text_layout(full, 1, lines));
    assert(strlen(lines[0]) == COMPACT_TEXT_COLUMNS);
    full[COMPACT_TEXT_COLUMNS] = 'B';
    full[COMPACT_TEXT_COLUMNS + 1] = '\0';
    assert(!compact_board_text_layout(full, 1, lines));
    assert(compact_board_text_layout(full, 4, lines));
    assert(strcmp(lines[1], "B") == 0);

    // Explicit line breaks after a full row consume exactly one line break.
    full[COMPACT_TEXT_COLUMNS] = '\n';
    full[COMPACT_TEXT_COLUMNS + 1] = 'B';
    full[COMPACT_TEXT_COLUMNS + 2] = '\0';
    assert(compact_board_text_layout(full, 4, lines));
    assert(strcmp(lines[1], "B") == 0);
    assert(compact_board_text_layout("one\ntwo\nthree\nfour", 4, lines));
    assert(strcmp(lines[3], "four") == 0);
    assert(!compact_board_text_layout("one\ntwo\nthree\nfour\nfive", 4, lines));

    assert(compact_board_text_layout("A message with a readable ending", 4, lines));
    assert(strcmp(lines[0], "A message with a readable") == 0);
    assert(strcmp(lines[1], "ending") == 0);
    assert(compact_board_text_layout("1234567890123456789012345 next", 4, lines));
    assert(strcmp(lines[0], "1234567890123456789012345") == 0);
    assert(strcmp(lines[1], "next") == 0);
    assert(compact_board_text_layout("1234567890123456789012345  next", 4, lines));
    assert(strcmp(lines[1], "next") == 0);
    assert(compact_board_text_layout("1234567890123456789012345  \nnext", 2, lines));
    assert(strcmp(lines[1], "next") == 0);
    assert(compact_board_text_layout("\n\n1234567890123456789012345  \nnext", 4, lines));
    assert(strcmp(lines[2], "1234567890123456789012345") == 0);
    assert(strcmp(lines[3], "next") == 0);
    assert(compact_board_text_layout("one two three four five six seven eight nine ten eleven twelve", 4, lines));
    assert(strcmp(lines[0], "one two three four five") == 0);
    assert(strcmp(lines[1], "six seven eight nine ten") == 0);
    assert(strcmp(lines[2], "eleven twelve") == 0);
    assert(!compact_board_text_layout("one two three four five six seven eight nine ten eleven twelve", 2, lines));
    assert(compact_board_text_layout("ABCDEFGHIJKLMNOPQRSTUVWXYZZ end", 4, lines));
    assert(strcmp(lines[0], "ABCDEFGHIJKLMNOPQRSTUVWXY") == 0);
    assert(strcmp(lines[1], "ZZ end") == 0);
    assert(compact_board_text_layout("one\n\ntwo", 4, lines));
    assert(strcmp(lines[1], "") == 0);
    assert(strcmp(lines[2], "two") == 0);

    memset(full, 'A', sizeof(full));
    full[COMPACT_TEXT_MAX] = '\0';
    assert(compact_board_text_layout(full, 4, lines));
    for (int row = 0; row < COMPACT_TEXT_ROWS; ++row) assert(strlen(lines[row]) == COMPACT_TEXT_COLUMNS);
    full[COMPACT_TEXT_MAX] = 'B';
    full[COMPACT_TEXT_MAX + 1] = '\0';
    assert(!compact_board_text_layout(full, 4, lines));
    puts("compact display text tests passed");
    return 0;
}
