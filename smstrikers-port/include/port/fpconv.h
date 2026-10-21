// Float to 32-bit integer the way the console's fctiwz converted: toward zero, saturating at the
// s32 limits, NaN as INT32_MIN. A C cast is undefined out of range, and arm64 and x86-64 answer
// differently.

#ifndef PORT_FPCONV_H
#define PORT_FPCONV_H

#include <stdint.h>

static inline int32_t PortFloatToS32(float f)
{
    if (f >= 2147483648.0f)
        return INT32_MAX;
    if (f > -2147483648.0f)
        return (int32_t)f;
    return INT32_MIN;
}

#endif   // PORT_FPCONV_H
