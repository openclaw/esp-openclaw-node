#include "dial_control.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    dial_control_t control;
    assert(dial_control_configure(&control, -100, 100, 5, -20));
    assert(dial_control_positions(&control) == 40);
    assert(dial_control_value(&control) == -20);
    assert(dial_control_move(&control, 40));
    assert(dial_control_value(&control) == 100);
    assert(!dial_control_move(&control, 41));
    assert(!dial_control_move(&control, -1));
    assert(!dial_control_move(&control, 40));
    assert(dial_control_move(&control, 0));
    assert(dial_control_value(&control) == -100);

    const double invalid[][4] = {
        {0, 100, 0, 50}, {100, 0, 1, 50}, {0, 0, 1, 0},
        {0, 100, 3, 51}, {0, 100, 5, 51}, {0, 100, 5, 105},
        {0, 100, 5, -5}, {0, 1001, 1, 0}, {0, 100, 1.5, 3},
        {NAN, 100, 1, 0}, {0, INFINITY, 1, 0}, {0, 100, 1, NAN},
        {-10001, 100, 1, 0}, {0, 10001, 1, 0}, {0, 100, INFINITY, 0},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!dial_control_configure(&control, invalid[i][0], invalid[i][1], invalid[i][2], invalid[i][3]));
        assert(dial_control_value(&control) == -100); // Rejected updates preserve the displayed control.
    }
    assert(dial_control_configure(&control, -10000, 10000, 20, 10000));
    assert(dial_control_positions(&control) == 1000);
    assert(dial_control_value(&control) == 10000);
    puts("dial control: passed");
}
