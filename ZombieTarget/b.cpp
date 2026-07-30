// TU B: its file-static "inner" is the record NO current code path should
// be attributed (same plain name, different body - the zombie shape).
#include <cstdint>

static uint64_t inner(uint64_t x)
{
    volatile uint64_t k = x;
    k += 0;
    k += 0;
    return k + 50;
}

uint64_t innerB(uint64_t x)
{
    volatile uint64_t keep = x;
    return inner(keep) + 1;
}
