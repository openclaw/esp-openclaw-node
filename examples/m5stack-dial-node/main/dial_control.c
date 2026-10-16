#include "dial_control.h"

#include <math.h>
#include <stddef.h>

static bool bounded_integer(double value, int minimum, int maximum)
{
    return isfinite(value) && value >= minimum && value <= maximum && trunc(value) == value;
}

bool dial_control_configure(dial_control_t *control, double minimum,
                            double maximum, double step, double value)
{
    if (control == NULL || !bounded_integer(minimum, -10000, 10000) ||
        !bounded_integer(maximum, -10000, 10000) ||
        !bounded_integer(step, 1, 20000) || !bounded_integer(value, -10000, 10000)) {
        return false;
    }
    int low = (int)minimum, high = (int)maximum, stride = (int)step, initial = (int)value;
    if (low >= high || initial < low || initial > high ||
        (high - low) % stride != 0 || (initial - low) % stride != 0 ||
        (high - low) / stride > 1000) {
        return false;
    }
    *control = (dial_control_t){low, high, stride, (initial - low) / stride};
    return true;
}

int dial_control_value(const dial_control_t *control)
{
    return control->minimum + control->position * control->step;
}

int dial_control_positions(const dial_control_t *control)
{
    return (control->maximum - control->minimum) / control->step;
}

bool dial_control_move(dial_control_t *control, int position)
{
    if (position < 0 || position > dial_control_positions(control) || position == control->position) {
        return false;
    }
    control->position = position;
    return true;
}
