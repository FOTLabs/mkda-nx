#include <assert.h>
#include <stdlib.h>
#include "recomp_funcs.h"
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_esp;
static uint32_t seen, correction;
extern recomp_func_t recomp_lookup_manual(uint32_t);
void sub_00024FB0(void) { seen = MEM32(g_esp + 4); g_esp += 4; }
void sub_002075C9(void) {
    MEM32(MEM32(g_esp + 4) + 0xC4) = correction;
    g_esp += 16;
}
void mkda_task_complete(void) {}
void mkda_task_replace(void) {}
void mkda_task_replace_private(void) {}
recomp_func_t mkda_task_lookup_entry(uint32_t va) { (void)va; return NULL; }
static void caption(uint32_t supplied, uint32_t expected) {
    g_esp = 0x100; MEM32(0x104) = supplied;
    recomp_lookup_manual(0x24FB0)();
    assert(seen == expected && g_esp == 0x104);
    assert(MEM32(0x104) == supplied); /* preserve caller's argument */
}
int main(void) {
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 0x300000);
    assert(g_xbox_mem_offset);
    caption(4800,4800); /* no decoder yet */
    const uint32_t decoder=0x1000;
    correction=2100; g_esp=0x100; MEM32(0x104)=decoder;
    recomp_lookup_manual(0x2075C9)();
    assert(g_esp==0x110 && MEM32(decoder+0xC4)==2100);
    MEM32(decoder+0xB8)=4790;
    caption(6890,4790); /* stall must not trigger 4800ms cue early */
    MEM32(decoder+0xB8)=4810; caption(6910,4810);
    caption(17,17); /* stale decoder at a movie transition */
    MEM32(decoder+0xC4)=0; caption(4810,4810);
    MEM32(decoder+0xC4)=(uint32_t)-50; caption(4760,4810);
    free((void *)g_xbox_mem_offset);
    return 0;
}
