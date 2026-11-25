#include <stdint.h>

uint32_t distance_u32(uint32_t a, uint32_t b)
{
    if (a >= b) return a - b;
    return b - a;
}

uint32_t add_u32(uint32_t a, uint32_t b)
{
    return a + b;
}
