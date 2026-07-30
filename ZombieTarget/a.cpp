// TU A: its file-static "inner" is the one current code executes.
#include <cstdint>

static uint64_t inner(uint64_t x)
{
    volatile uint64_t k = x;
    return k + 5;
}

uint64_t innerA(uint64_t x)
{
    volatile uint64_t keep = x;
    return inner(keep) + 1;
}
