// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include "compact_input.h"

static compact_input_event_t sample(compact_input_t *input, uint32_t now,
                                    uint32_t revision, bool a, bool b)
{
    return compact_input_sample(input, now, revision, a, b, false, 3);
}

int main(void)
{
    compact_input_t input = {0};
    assert(!sample(&input, 0, 1, true, false).select);
    assert(!sample(&input, 1000, 1, true, false).home);
    sample(&input, 1010, 1, false, false);
    assert(!sample(&input, 1040, 1, false, false).select);
    sample(&input, 1100, 1, true, false);
    sample(&input, 1130, 1, true, false);
    sample(&input, 1200, 1, false, false);
    assert(sample(&input, 1230, 1, false, false).select);
    assert(!sample(&input, 1300, 1, false, false).select);

    // Bounce and a page replacement cannot submit a stale press.
    sample(&input, 1400, 1, true, false);
    sample(&input, 1410, 1, false, false);
    assert(!sample(&input, 1450, 1, false, false).select);
    sample(&input, 1500, 1, true, false);
    sample(&input, 1530, 1, true, false);
    sample(&input, 1600, 2, false, false);
    assert(!sample(&input, 1630, 2, false, false).select);

    sample(&input, 1700, 2, true, false);
    sample(&input, 1730, 2, true, false);
    assert(sample(&input, 2400, 2, true, false).home);
    assert(!sample(&input, 2500, 2, true, false).home);
    sample(&input, 2600, 2, false, false);
    assert(!sample(&input, 2630, 2, false, false).select);

    sample(&input, 2700, 2, false, true);
    sample(&input, 2730, 2, false, true);
    sample(&input, 2800, 2, false, false);
    assert(sample(&input, 2830, 2, false, false).move == 1);
    sample(&input, 2900, 2, false, true);
    sample(&input, 2930, 2, false, true);
    assert(sample(&input, 3600, 2, false, true).move == -1);
    sample(&input, 3700, 2, false, false);
    assert(!sample(&input, 3730, 2, false, false).move);

    input = (compact_input_t){0};
    compact_input_sample(&input, 0, 1, false, false, true, 3);
    compact_input_sample(&input, 30, 1, false, false, true, 3);
    const uint8_t forward[] = {1, 0, 2, 3};
    for (unsigned i = 0; i < 4; ++i) {
        compact_input_event_t event = compact_input_sample(&input, 40 + i, 1, false, false, true, forward[i]);
        assert(event.move == (i == 3 ? 1 : 0));
    }
    const uint8_t reverse[] = {2, 0, 1, 3};
    for (unsigned i = 0; i < 4; ++i) {
        compact_input_event_t event = compact_input_sample(&input, 50 + i, 1, false, false, true, reverse[i]);
        assert(event.move == (i == 3 ? -1 : 0));
    }
    // A two-bit jump is not a detent, and reversing contact bounce cancels.
    assert(!compact_input_sample(&input, 60, 1, false, false, true, 0).move);
    assert(!compact_input_sample(&input, 61, 1, false, false, true, 3).move);
    assert(!compact_input_sample(&input, 62, 1, false, false, true, 1).move);
    assert(!compact_input_sample(&input, 63, 1, false, false, true, 3).move);
    puts("Compact input: debounce, startup holds, stale pages, long presses and quadrature passed");
}
