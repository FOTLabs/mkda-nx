#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_esp;
extern void sub_001CC220(void);

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 4096);
    assert(g_xbox_mem_offset);
    unsigned char expected[128];
    for (unsigned source = 0; source < 8; ++source)
        for (unsigned destination = 0; destination < 8; ++destination)
            for (unsigned count = 0; count < 64; ++count) {
                for (unsigned index = 0; index < sizeof(expected); ++index)
                    expected[index] = MEM8(0x200 + index) = (unsigned char)index;
                memmove(expected + destination, expected + source, count);
                g_esp = 0x100;
                MEM32(g_esp) = 0x12345678;
                MEM32(g_esp + 4) = 0x200 + destination;
                MEM32(g_esp + 8) = 0x200 + source;
                MEM32(g_esp + 12) = count;
                sub_001CC220();
                assert(g_esp == 0x104 && g_eax == 0x200 + destination);
                assert(MEM32(0x100) == 0x12345678);
                assert(memcmp(expected, (void *)XBOX_PTR(0x200), sizeof(expected)) == 0);
            }
    return 0;
}
