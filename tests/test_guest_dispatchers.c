/* Run the actual lifted dispatcher bodies, including their stack/FP ABI. */
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_ebp, g_seh_ebp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top, g_fp_cmp, g_df;
RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile int g_gil_contended;
volatile uint64_t g_icall_count;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_trace_idx;
uint32_t g_xbox_code_lo = 0x1000, g_xbox_code_hi = 0x300000;
extern void mkda_task_complete(void);
extern void mkda_task_replace(void);
extern void mkda_task_replace_private(void);
extern recomp_func_t mkda_task_lookup_entry(uint32_t address);
static int shared, plain, progress, callbacks, corrupt_stack_padding;
static int entry_calls, reset_on_yield;

static void deleting_callback(void)
{
    MEM32(MEM32(0x343648) + 4) = 0;
    g_esp += 4;
}

static void scheduler_callback(void)
{
    PUSH32(g_esp, g_ebp);
    g_ebp = g_seh_ebp = g_esp;
    g_esp -= 12;
    PUSH32(g_esp, 0xd4c62);
    sub_000E1780();
    abort(); /* The original helper never resumes this callback. */
}

void recomp_preempt(void) {}
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; abort(); }
void recomp_spin_wake(void) {}
void recomp_spin_yield(void) {}
void recomp_icall_fail_log(uint32_t address) { (void)address; abort(); }
void recomp_icall_not_code_log(uint32_t address) { (void)address; abort(); }
void kernel_thunk_dispatch(uint32_t address) { (void)address; abort(); }

static void callback(void)
{
    uint32_t return_sp = g_esp;
    if (shared) assert(return_sp <= MEM32(0x2f7184));
    if (shared && return_sp == MEM32(0x2f7184) - 4)
        assert(g_ebp == MEM32(0x2f7194));
    PUSH32(g_esp, g_ebp);
    g_ebp = g_esp;
    g_seh_ebp = g_ebp;
    PUSH32(g_esp, 0x104198);
    PUSH32(g_esp, g_ebp);
    ++callbacks;
    if (reset_on_yield) {
        uint32_t task = MEM32(0x343648);
        MEM32(task + 0x44) = MEM32(task + 0x40);
    }
    if (corrupt_stack_padding) MEM8(0x40000 - 0x4001) ^= 1;
    g_esp = return_sp + 4;
}

static void task_entry(void)
{
    ++entry_calls;
    uint32_t return_sp = g_esp;
    PUSH32(g_esp, g_ebp);
    uint32_t frame = g_esp;
    g_ebp = frame; g_seh_ebp = frame;
    g_esp -= 32;
    MEM32(g_esp) = 0xabcdef01;
    for (int i = 0; i < 2; ++i) {
        ++progress;
        g_ebp = frame; g_seh_ebp = frame;
        PUSH32(g_esp, 0x100010);
        if (shared) sub_000E1A30(); else sub_000E1C80();
        assert(g_esp == frame - 32);
        assert(g_ebp == frame && g_seh_ebp == frame);
        assert(MEM32(g_esp) == 0xabcdef01);
    }
    ++progress;
    g_esp = return_sp + 4;
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = 1.0;
}

static void replacing_entry(void)
{
    ++progress;
    PUSH32(g_esp, 0x100030);
    if (shared) sub_000E1A30(); else sub_000E1C80();
    PUSH32(g_esp, shared ? 0x3f800000 : 0); /* zero restarts the private entry now */
    PUSH32(g_esp, 0xe0bd0);
    PUSH32(g_esp, 0x100020);
    uint32_t target = shared ? 0xe1b10 : 0xe1d30;
    recomp_func_t replace = recomp_lookup_manual(target);
    if (!replace) replace = shared ? sub_000E1B10 : sub_000E1D30;
    replace();
    /* Xbox RET resumes the dispatcher, never this abandoned task frame. */
    abort();
}

recomp_func_t recomp_lookup(uint32_t address)
{
    if (address == 0x100000) return task_entry;
    if (address == 0x100100) return callback;
    if (address == 0xe0bd0) return sub_000E0BD0;
    if (address == 0x100200) return replacing_entry;
    if (address == 0x100300) return scheduler_callback;
    if (address == 0x100400) return deleting_callback;
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t address)
{
    if (address == 0xe1765) return mkda_task_complete;
    if (address == 0xe1b10) return mkda_task_replace;
    if (address == 0xe1d30) return mkda_task_replace_private;
    return mkda_task_lookup_entry(address);
}
recomp_func_t recomp_lookup_kernel(uint32_t address) { (void)address; return NULL; }

static void dispatch(void)
{
    MEM32(0x343648) = 0x1000;
    g_esp = 0x8000; g_ebp = 0x8080; g_seh_ebp = 0x8080;
    MEM32(g_esp) = 0xe1765;
    for (uint32_t a = 0x8004; a < 0x8088; a += 4) MEM32(a) = a ^ 0x13579bdf;
    if (plain) sub_000E1800();
    else if (shared) sub_000E18C0(); else sub_000E1B50();
    assert(g_esp == 0x8004 && g_ebp == 0x8080);
    for (uint32_t a = 0x8004; a < 0x8088; a += 4)
        assert(MEM32(a) == (a ^ 0x13579bdf));
}

int main(void)
{
    assert(setenv("MKDA_TASK_STACK_GUARD", "1", 1) == 0);
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 8 * 1024 * 1024);
    assert(g_xbox_mem_offset);
    /* E1580's pre-dispatch callback can exit before a fiber is active. */
    MEM32(0x343648) = 0x1000;
    MEM32(0x1000) = 0x2000;
    MEM32(0x2010) = 0x100400;
    MEM32(0x1028) = 0x100300;
    MEM32(0x2f7174) = 0xe1765;
    MEM32(0x2f7184) = 0x8004;
    MEM32(0x2f7194) = 0x8080;
    for (uint32_t a = 0x8004; a < 0x8088; a += 4) MEM32(a) = a ^ 0x13579bdf;
    MEM32(0x1028) = 0x100100;
    g_esp = 0x8000; g_ebp = g_seh_ebp = 0x8080;
    MEM32(g_esp) = 0xe174d;
    recomp_func_t normal_callback = recomp_lookup_manual(0x100100);
    assert(normal_callback);
    normal_callback();
    assert(callbacks == 1 && g_esp == 0x8004);
    MEM32(0x1028) = 0x100300;
    for (int explicit_callback = 0; explicit_callback < 2; ++explicit_callback) {
        MEM32(0x1004) = 1;
        MEM32(0x102c) = explicit_callback ? 0x100400 : 0;
        g_esp = 0x8000; g_ebp = g_seh_ebp = 0x8080;
        MEM32(g_esp) = 0xe174d;
        recomp_func_t pre_dispatch = recomp_lookup_manual(0x100300);
        if (!pre_dispatch) pre_dispatch = recomp_lookup(0x100300);
        pre_dispatch();
        assert(MEM32(0x1004) == 0 && g_esp == 0x8004);
        assert(g_ebp == 0x8080 && g_seh_ebp == 0x8080);
        for (uint32_t a = 0x8004; a < 0x8088; a += 4)
            assert(MEM32(a) == (a ^ 0x13579bdf));
    }
    for (int mode = 0; mode < 3; ++mode) {
        shared = mode != 0;
        plain = mode == 2;
        progress = callbacks = 0;
        MEM32(0x1000 + 4) = 1;
        MEM32(0x1000 + 0x30) = 0x100000;
        MEM32(0x1000 + 0x2c) = 0x100100;
        MEM32(0x1000 + 0x34) = 0;
        MEM32(0x1000 + 0x40) = 0x40000;
        MEM32(0x1000 + 0x44) = 0x40000;
        dispatch(); assert(progress == 1);
        dispatch(); assert(progress == 2);
        dispatch(); assert(progress == 3 && callbacks == 3);
    }
    /* The real constant-return entry that first tripped the boot frame guard. */
    shared = 1;
    plain = 0;
    callbacks = 0;
    MEMF(0x23715c) = 1000.0f;
    MEM32(0x1030) = 0xe0bd0;
    MEM32(0x1034) = 0;
    MEM32(0x1044) = MEM32(0x1040);
    dispatch();
    assert(callbacks == 1);
    for (int mode = 0; mode < 3; ++mode) {
        shared = mode != 0;
        plain = mode == 2;
        callbacks = progress = 0;
        MEM32(0x1030) = 0x100200;
        MEM32(0x1034) = 0;
        MEM32(0x1044) = MEM32(0x1040);
        dispatch();
        assert(progress == 1 && callbacks == 1);
        dispatch();
        assert(progress == 1 && callbacks == 2);
        assert(MEM32(0x1030) == 0xe0bd0 && MEMF(0x100c) == (shared ? 1.0f : 1000.0f));
    }
    for (shared = 0; shared <= 1; ++shared) {
        plain = 0;
        entry_calls = 0;
        MEM32(0x1004) = 3;
        MEM32(0x1030) = 0x100000;
        MEM32(0x1034) = 0;
        MEM32(0x1044) = MEM32(0x1040);
        reset_on_yield = 1;
        dispatch();
        assert(entry_calls == 1);
        reset_on_yield = 0;
        dispatch();
        assert(entry_calls == 2); /* same entry/resume, callback restarted ESP */
    }
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        shared = plain = 0;
        corrupt_stack_padding = 1;
        MEM32(0x1004) = 2;
        MEM32(0x1030) = 0x100000;
        MEM32(0x1034) = 0;
        MEM32(0x1044) = MEM32(0x1040);
        dispatch();
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    return 0;
}
