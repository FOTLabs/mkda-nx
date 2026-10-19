#include <assert.h>
#include <stdlib.h>
#include <setjmp.h>
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_ebp, g_seh_ebp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top, g_fp_cmp;
RECOMP_TLS int g_df;
RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
static int progress[4];
static int entries[4];
static jmp_buf main_checkpoint;
static int abandon_next;
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; abort(); }
static void yield_callback(void)
{
    /* Lifted framed callbacks publish their own EBP/SEH frame. The original
     * yield restores the task's pre-callback registers on its next dispatch. */
    g_ebp = 0xbaad;
    g_seh_ebp = 0xdead;
    recomp_set_mxcsr(0x1f80);
    g_esp += 4;
}
extern void mkda_task_on_longjmp(uint32_t guest_buffer);
extern void sub_000E1B50(void), sub_000E1C80(void);
extern void sub_000E18C0(void), sub_000E1A30(void);
extern void sub_000E1CF0(void), sub_000E1D10(void);
static void run_task(int shared_stack);
void sub_000E1800_gen(void) { sub_000E1B50_gen(); }
void sub_000E18C0_gen(void) { run_task(1); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup(uint32_t address) { return address == 0x5000 ? yield_callback : NULL; }

/* A synthetic guest task: yields with live native locals and guest registers. */
void sub_000E1B50_gen(void)
{
    run_task(0);
}

static void run_task(int shared_stack)
{
    uint32_t task = MEM32(0x343648);
    unsigned index = (task - 0x1000) / 0x100;
    volatile unsigned native_local = 0x500 + index;
    ++entries[index];
    ++progress[index];
    if (abandon_next) {
        mkda_task_on_longjmp(0x2f6e18);
        longjmp(main_checkpoint, 1);
    }
    uint32_t stack_start = shared_stack ? 0x7000 + index * 0x100 : 0x10000 + index * 0x1000;
    g_esp = stack_start;
    MEM32(g_esp) = 0x12340000 + index;
    MEM32(g_esp + 0x10) = 0xabcdef00 + index;
    g_ebx = 0x600 + index;
    g_ebp = 0x700 + index;
    g_seh_ebp = 0x700 + index;
    g_fp_stack[0] = 42.0 + index;
    uint32_t control = 0x9f80 | (index << 13);
    recomp_set_mxcsr(control);
    if (shared_stack) sub_000E1A30(); else sub_000E1C80();
    assert(native_local == 0x500 + index);
    assert(g_ebx == 0x600 + index);
    assert(g_ebp == 0x700 + index);
    assert(g_seh_ebp == 0x700 + index);
    assert(g_esp == stack_start + 4);
    assert(MEM32(stack_start + 0x10) == 0xabcdef00 + index);
    assert(g_fp_stack[0] == 42.0 + index);
    assert(recomp_get_mxcsr() == control);
    ++progress[index];
}

static void dispatch(unsigned index)
{
    MEM32(0x343648) = 0x1000 + index * 0x100;
    g_esp = 0x8000;
    MEM32(g_esp) = 0xe1765;
    g_ebx = 0x11112222;
    g_ebp = 0x8040;
    g_seh_ebp = 0x8040;
    for (uint32_t address = 0x8004; address < 0x8048; address += 4)
        MEM32(address) = address ^ 0x13579bdf;
    g_fp_stack[0] = 99.0;
    recomp_set_mxcsr(0x1f80);
    if (index >= 2) sub_000E18C0(); else sub_000E1B50();
    assert(g_esp == 0x8004);
    assert(g_ebx == 0x11112222);
    assert(g_ebp == 0x8040 && g_seh_ebp == 0x8040);
    for (uint32_t address = 0x8004; address < 0x8048; address += 4)
        assert(MEM32(address) == (address ^ 0x13579bdf));
    assert(g_fp_stack[0] == 99.0);
    assert(recomp_get_mxcsr() == 0x1f80);
}

int main(void)
{
    assert(setenv("MKDA_TASK_STACK_GUARD", "1", 1) == 0);
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 8 * 1024 * 1024);
    assert(g_xbox_mem_offset);
    MEM32(0x1000 + 0x2c) = 0x5000;
    MEM32(0x1200 + 0x2c) = 0x5000;
    dispatch(0); dispatch(1);
    assert(progress[0] == 1 && progress[1] == 1);
    dispatch(0); dispatch(1);
    assert(progress[0] == 2 && progress[1] == 2);
    dispatch(0);
    assert(progress[0] == 3); /* completed task slot can be reused */
    assert(entries[0] == 2);
    MEM32(0x1000 + 0x34) = 0; /* recycle a task while its old fiber is suspended */
    dispatch(0);
    assert(entries[0] == 3); /* new entry, not the stale native continuation */
    dispatch(0);
    assert(progress[0] == 5);
    dispatch(0);
    assert(entries[0] == 4);
    /* E1310 assigns a new task ID even if recycled entry/resume values match. */
    MEM32(0x1004) = 0xffff0001;
    dispatch(0);
    assert(entries[0] == 5);
    dispatch(0);
    MEM32(0x1200 + 0x40) = 0x40000;
    MEM32(0x1300 + 0x40) = 0x41000;
    dispatch(2); dispatch(3);
    assert(progress[2] == 1 && progress[3] == 1);
    memset((void *)XBOX_PTR(0x6000), 0xee, 0x2004);
    dispatch(2); dispatch(3);
    assert(progress[2] == 2 && progress[3] == 2);
    g_esp = 0x12000;
    g_ebp = 0x45678;
    MEM32(g_esp) = 0xcbcc3;
    MEM32(0x2f7184) = 0x8004;
    sub_000E1CF0();
    assert(g_esp == 0x8004 && MEM32(0x2f7188) == 0x12004);
    MEM32(g_esp) = 0x123456;
    sub_000E1D10();
    assert(g_esp == 0x12004 && MEM32(0x2f7188) == 0);
    assert(g_ebp == 0x45678 && MEM32(0x2f7170) == 0x123456);
    if (!setjmp(main_checkpoint)) {
        abandon_next = 1;
        dispatch(2);
        assert(!"task should have jumped to the main checkpoint");
    }
    abandon_next = 0;
    dispatch(2); /* abandoned native frames must not leave the task active */
    return 0;
}
