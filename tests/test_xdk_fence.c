#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_ecx;
static unsigned original_calls, registrations;
static uint32_t registered;
static unsigned parse_calls, commit_calls, dsp_registrations;
static uint32_t dsp_registered;
void sub_001DA850_gen(void) { ++original_calls; }
void sub_001EBAD7_gen(void) { ++parse_calls; }
void sub_001EBC73_gen(void) { ++commit_calls; }
int xbox_ApuDspAckWord(uint32_t address)
{
    ++dsp_registrations;
    dsp_registered = address;
    return 0;
}
void nv2a_pb_set_semaphore_target(uint32_t address)
{
    ++registrations;
    registered = address;
}
extern void sub_001DA850(void);
extern void sub_001EBAD7(void), sub_001EBC73(void);

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 8 * 1024 * 1024);
    assert(g_xbox_mem_offset);
    /* push esi; mov esi,[abs32]; mov eax,[esi+34]; mov ecx,[eax] */
    unsigned char prologue[] = {0x56, 0x8b, 0x35, 0xd0, 0x4f, 0x1e, 0,
                                0x8b, 0x46, 0x34, 0x8b, 0x08};
    memcpy((void *)XBOX_PTR(0x1da850), prologue, sizeof(prologue));
    MEM32(0x1e4fd0) = 0x400000;
    MEM32(0x400034) = 0x700000;
    sub_001DA850();
    assert(original_calls == 1 && registrations == 1 && registered == 0x700000);
    sub_001DA850();
    assert(original_calls == 2 && registrations == 1);
    MEM32(0x400034) = 0x700004;
    sub_001DA850();
    assert(original_calls == 3 && registrations == 2 && registered == 0x700004);
    g_ecx = 0x401000;
    MEM32(0x401008) = 0x402000;
    MEM32(0x402010) = 0x403000;
    MEM32(0x403000) = 0x600000;
    sub_001EBAD7(); sub_001EBC73();
    assert(parse_calls == 1 && commit_calls == 1);
    assert(dsp_registrations == 2 && dsp_registered == 0x600810);
    MEM32(0x401008) = 0;
    sub_001EBAD7();
    assert(parse_calls == 2 && dsp_registrations == 2);
    return 0;
}
