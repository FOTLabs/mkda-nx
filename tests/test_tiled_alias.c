#include <assert.h>
#define RECOMP_SOFTWARE_TILED_ALIAS
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

int main(void)
{
    const uint32_t unchanged[] = {0, 0x03ffffff, 0x80000000, 0x83ffffff,
                                  0xefffffff, 0xf4000000, 0xfd000000, 0xffffffff};
    for (unsigned i = 0; i < sizeof unchanged / sizeof *unchanged; ++i)
        assert(recomp_memory_address(unchanged[i]) == unchanged[i]);
    assert(recomp_memory_address(0xf0000000) == 0x80000000);
    assert(recomp_memory_address(0xf3ffffff) == 0x83ffffff);
    assert(recomp_memory_address(0xf0ba1000) == 0x80ba1000);
    uint32_t storage[4] = {0};
    g_xbox_mem_offset = (ptrdiff_t)storage - (ptrdiff_t)0x80000000u;
    MEM32(0xf0000004) = 0x12345678;
    assert(MEM32(0x80000004) == 0x12345678);
    MEM8(0x80000004) = 0xab;
    assert(MEM8(0xf0000004) == 0xab);
    assert(XBOX_PTR(0x1f0000004ull) == XBOX_PTR(0xf0000004));
    return 0;
}
