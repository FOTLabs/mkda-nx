#include <assert.h>
#define RECOMP_GENERATED_CODE
/* Exercise the runtime's supported nonvolatile performance configuration as
 * well; volatile default accesses otherwise hide this compiler assumption. */
#define RECOMP_MEM_VOLATILE
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;

/* Both addresses may name the same bytes in guest RAM. In particular,
 * memcpy-style integer writes can change values later read as floats. */
__attribute__((noinline, noclone))
static float guest_write_read(uint32_t float_address, uint32_t integer_address)
{
    MEMF(float_address) = 1.0f;
    MEM32(integer_address) = 0;
    return MEMF(float_address);
}

__attribute__((noinline, noclone))
static int wrapped_increment(int32_t value)
{
    return value + 1 < value;
}

int main(void)
{
    uint32_t storage[4] = {0};
    g_xbox_mem_offset = (ptrdiff_t)storage;
    volatile uint32_t address = 4;
    assert(guest_write_read(address, address) == 0.0f);
    assert(guest_write_read(4, 8) == 1.0f);
    assert(wrapped_increment(INT32_MAX));
    assert(!wrapped_increment(0));
    return 0;
}
