/* XMATH.OBJ: scalar helpers. */
#include "port.h"

/* Clamp v into [lo, hi]. NaN falls through to lo, as the x87 original does. */
float xmath_LimitRange(float v, float lo, float hi)
{
    if (v > hi)
        return hi;
    if (!(v >= lo))
        return lo;
    return v;
}
HY_PORT(xmath_LimitRange)

/* Move from `from` towards `to` by at most `max_step`. */
float xmath_LimitChange(float to, float from, float max_step)
{
    float d = to - from;
    float mag = d < 0.0f ? -d : d;
    if (mag > max_step)
        return d > 0.0f ? from + max_step : from - max_step;
    return to;
}
HY_PORT(xmath_LimitChange)

/* Strictly between a and b, whichever order they come in. */
int xmath_IsFloatInARange(float x, float a, float b)
{
    if (a < b)
        return a < x && x < b;
    return x < a && b < x;
}
HY_PORT(xmath_IsFloatInARange)
