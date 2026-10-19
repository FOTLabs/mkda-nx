#define _XOPEN_SOURCE 700
#include "native_context.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* MKDA's cooperative scheduler, verified from 0xE1B50/0xE1C80:
 * 343648=current task; task+10..1c=callee registers, +34=resume PC,
 * +40=stack base, +44=suspended ESP. The dispatch caller's continuation
 * is saved at 2f7174. Native stacks must survive a yield as well. */
typedef struct GuestState {
    uint32_t a, b, c, d, si, di, sp, bp, seh;
    uint32_t mxcsr;
    double fp[8];
    int top, cmp, direction;
    uint16_t control, cc;
    RecompXmm xmm[8];
    RecompMmx mm[8];
} GuestState;

typedef struct TaskFiber {
    uint32_t task, task_id;
    int suspended;
    NativeContext native;
    void *stack;
    GuestState guest;
    recomp_func_t entry;
    void *guest_stack_copy;
    uint32_t guest_stack_start, guest_stack_size, guest_stack_top;
    uint32_t suspended_entry, suspended_resume;
    uint32_t suspended_sp, suspended_backing;
    uint32_t trace_resume[16], trace_count, trace_entry;
    jmp_buf entry_return;
    int entry_armed;
    uint32_t stack_header, stack_allocation_size, stack_allocation_top, stack_padding;
    struct TaskFiber *next;
} TaskFiber;

static _Thread_local TaskFiber *fibers, *active;
typedef struct SchedulerCallback {
    jmp_buf return_to_scheduler;
    uint32_t sp, bp, seh;
    struct SchedulerCallback *previous;
} SchedulerCallback;
static _Thread_local SchedulerCallback *scheduler_callback;
static _Thread_local NativeContext scheduler;
static _Thread_local GuestState scheduler_guest;
static _Thread_local unsigned guard_checks, guard_skips;
static _Thread_local unsigned task_replacements;
static _Thread_local unsigned private_task_replacements;
static _Thread_local int header_guard_enabled;
static _Thread_local uint32_t last_task, last_scheduler_sp, last_scheduler_bp;

static void check_task_stack_header(void)
{
    if (!header_guard_enabled) return;
    for (TaskFiber *owner = fibers; owner; owner = owner->next) {
        if (!owner->stack_header || MEM32(owner->task + 4) != owner->task_id) continue;
        uint32_t size = MEM32(owner->stack_header + 8);
        uint32_t padding = MEM8(owner->stack_allocation_top - 0x4001);
        if (size == owner->stack_allocation_size && padding == owner->stack_padding) continue;
        fprintf(stderr, "[MKDA] Task %08X stack header %08X changed size %08X -> %08X"
                        " padding=%X -> %X entry=%08X resume=%08X top=%08X sp=%08X\n",
                owner->task, owner->stack_header, owner->stack_allocation_size, size,
                owner->stack_padding, padding,
                MEM32(owner->task + 0x30), MEM32(owner->task + 0x34),
                owner->stack_allocation_top, g_esp);
        fprintf(stderr, "  active=%08X active-entry=%08X active-top=%08X active-saved=%08X last=%08X\n",
                active ? active->task : 0,
                active ? MEM32(active->task + 0x30) : 0,
                active ? MEM32(active->task + 0x40) : 0,
                active ? MEM32(active->task + 0x44) : 0, last_task);
        abort();
    }
}

/* E1B10/E1D30 replace a task and RET onto its dispatcher's saved guest stack.
 * A normal C return would continue the abandoned task's native frames. */
static void replace_task(int private_stack)
{
    if (!active || !active->entry_armed) {
        fprintf(stderr, "[MKDA] Task replacement outside entry trampoline\n");
        abort();
    }
    uint32_t task = MEM32(0x343648);
    uint32_t entry = MEM32(g_esp + 4), value = MEM32(g_esp + 8);
    uint32_t top = private_stack ? MEM32(task + 0x40) : MEM32(0x2f7184);
    ++task_replacements;
    if (private_stack) ++private_task_replacements;
    if (task_replacements <= 4 || (private_stack && private_task_replacements <= 4))
        fprintf(stderr, "[MKDA] Replace task %08X entry %08X -> %08X, sp=%08X -> %08X (%s)\n",
                task, MEM32(task + 0x30), entry, g_esp, top,
                private_stack ? "private" : "scheduler");
    MEM32(task + 0x30) = entry;
    MEM32(0x2f716c) = value;
    g_ecx = entry; g_edx = value;
    g_eax = top - 4;
    g_esp = g_eax + 4;
    g_ebp = g_seh_ebp = MEM32(0x2f7194);
    float result;
    memcpy(&result, &value, sizeof(result));
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = result;
    longjmp(active->entry_return, 1);
}

void mkda_task_replace(void) { replace_task(0); }
/* E1D30 has E1B10's ABI but RETs to the private stack's entry-call slot. */
void mkda_task_replace_private(void) { replace_task(1); }

static void task_entry_trampoline(void)
{
    uint32_t entry = MEM32(MEM32(0x343648) + 0x30);
    uint32_t entry_sp = g_esp;
    if (header_guard_enabled && MEM32(g_esp) == 0xe1ba2 &&
        g_esp + 4 != MEM32(active->task + 0x40)) {
        fprintf(stderr, "[MKDA] Private task %08X entry %08X starts with wrong ESP:"
                        " sp=%08X top=%08X saved=%08X\n", active->task, entry,
                g_esp, MEM32(active->task + 0x40), MEM32(active->task + 0x44));
        abort();
    }
    recomp_func_t function = recomp_lookup(entry);
    if (!function || active->entry_armed) abort();
    active->entry_armed = 1;
    if (!setjmp(active->entry_return)) {
        function();
        if (header_guard_enabled && g_esp != entry_sp + 4) {
            fprintf(stderr, "[MKDA] Task %08X entry %08X returned with wrong ESP:"
                            " %08X -> %08X expected=%08X\n",
                    active->task, entry, entry_sp, g_esp, entry_sp + 4);
            abort();
        }
    }
    check_task_stack_header();
    active->entry_armed = 0;
}

static void scheduler_callback_trampoline(void)
{
    uint32_t task = MEM32(0x343648);
    recomp_func_t function = recomp_lookup(MEM32(task + 0x28));
    if (!function) abort();
    SchedulerCallback frame;
    frame.sp = g_esp + 4;
    frame.bp = g_ebp; frame.seh = g_seh_ebp;
    frame.previous = scheduler_callback;
    scheduler_callback = &frame;
    /* E1780 exits from the pre-dispatch callback too. Its tail jump resumes
     * E1580 at E1765, bypassing all frames of the callback. Establish this
     * iteration's root stack even if no task dispatcher has run yet. */
    MEM32(0x2f7174) = 0xe1765;
    MEM32(0x2f7184) = frame.sp;
    MEM32(0x2f7194) = frame.bp;
    if (!setjmp(frame.return_to_scheduler)) function();
    scheduler_callback = frame.previous;
}

recomp_func_t mkda_task_lookup_entry(uint32_t address)
{
    check_task_stack_header();
    uint32_t task = MEM32(0x343648);
    if (!active && task && address == MEM32(task + 0x28) && MEM32(g_esp) == 0xe174d)
        return scheduler_callback_trampoline;
    if (!active || active->entry_armed) return NULL;
    uint32_t return_pc = MEM32(g_esp);
    if (address == MEM32(MEM32(0x343648) + 0x30) &&
        (return_pc == 0xe181a || return_pc == 0xe1991 || return_pc == 0xe1ba2))
        return task_entry_trampoline;
    return NULL;
}

void mkda_task_debug_dump(void)
{
    fprintf(stderr, "  [TASK] guard checks=%u skips=%u replacements=%u private=%u last=%08X sp=%08X bp=%08X\n",
            guard_checks, guard_skips, task_replacements, private_task_replacements,
            last_task, last_scheduler_sp, last_scheduler_bp);
}

/* The main loop's setjmp at 0xCB889 uses this buffer. A level transition
 * can longjmp there from a task, bypassing dispatch_task's native frames.
 * Do not free the currently executing native stack before longjmp leaves it. */
void mkda_task_on_longjmp(uint32_t guest_buffer)
{
    if (guest_buffer == 0x2f6e18) scheduler_callback = NULL;
    if (active && guest_buffer == 0x2f6e18) {
        fprintf(stderr, "[MKDA] Abandon task %08X at main-loop longjmp\n", active->task);
        active->suspended = 0;
        active = NULL;
    }
}
extern void sub_000E1B50_gen(void);
extern void sub_000E1800_gen(void);
extern void sub_000E18C0_gen(void);

static void save_guest(GuestState *state)
{
    state->mxcsr = recomp_get_mxcsr();
    state->a = g_eax; state->b = g_ebx; state->c = g_ecx; state->d = g_edx;
    state->si = g_esi; state->di = g_edi; state->sp = g_esp;
    state->bp = g_ebp; state->seh = g_seh_ebp;
    memcpy(state->fp, g_fp_stack, sizeof(state->fp));
    state->top = g_fp_top; state->cmp = g_fp_cmp;
    state->control = g_fp_control_word; state->cc = g_fp_cc;
    state->direction = g_df;
    state->xmm[0] = g_xmm0; state->xmm[1] = g_xmm1;
    state->xmm[2] = g_xmm2; state->xmm[3] = g_xmm3;
    state->xmm[4] = g_xmm4; state->xmm[5] = g_xmm5;
    state->xmm[6] = g_xmm6; state->xmm[7] = g_xmm7;
    state->mm[0] = g_mm0; state->mm[1] = g_mm1;
    state->mm[2] = g_mm2; state->mm[3] = g_mm3;
    state->mm[4] = g_mm4; state->mm[5] = g_mm5;
    state->mm[6] = g_mm6; state->mm[7] = g_mm7;
}

static void restore_guest(const GuestState *state)
{
    recomp_set_mxcsr(state->mxcsr);
    g_eax = state->a; g_ebx = state->b; g_ecx = state->c; g_edx = state->d;
    g_esi = state->si; g_edi = state->di; g_esp = state->sp;
    g_ebp = state->bp; g_seh_ebp = state->seh;
    memcpy(g_fp_stack, state->fp, sizeof(state->fp));
    g_fp_top = state->top; g_fp_cmp = state->cmp;
    g_fp_control_word = state->control; g_fp_cc = state->cc;
    g_df = state->direction;
    g_xmm0 = state->xmm[0]; g_xmm1 = state->xmm[1];
    g_xmm2 = state->xmm[2]; g_xmm3 = state->xmm[3];
    g_xmm4 = state->xmm[4]; g_xmm5 = state->xmm[5];
    g_xmm6 = state->xmm[6]; g_xmm7 = state->xmm[7];
    g_mm0 = state->mm[0]; g_mm1 = state->mm[1];
    g_mm2 = state->mm[2]; g_mm3 = state->mm[3];
    g_mm4 = state->mm[4]; g_mm5 = state->mm[5];
    g_mm6 = state->mm[6]; g_mm7 = state->mm[7];
}

void mkda_task_complete(void)
{
    if (!active) {
        if (scheduler_callback) {
            /* Return to E174D, whose verified task-ID-zero branch immediately
             * reaches E1765. Do not return through the abandoned callback. */
            uint32_t task = MEM32(0x343648);
            if (MEM32(task + 4)) abort();
            g_esp = scheduler_callback->sp;
            g_ebp = scheduler_callback->bp;
            g_seh_ebp = scheduler_callback->seh;
            longjmp(scheduler_callback->return_to_scheduler, 1);
        }
        uint32_t task = MEM32(0x343648);
        fprintf(stderr, "[MKDA] Task exit outside coroutine task=%08X table=%08X\n",
                task, MEM32(task));
        for (unsigned index = 0; index < 20; ++index)
            fprintf(stderr, "  task+%02X=%08X\n", index * 4, MEM32(task + index * 4));
        abort();
    }
    active->suspended = 0;
    restore_guest(&scheduler_guest);
    if (mkda_context_jump(&scheduler)) abort();
    abort();
}

static void task_start(void)
{
    restore_guest(&active->guest);
    active->entry();
    mkda_task_complete();
}

static void dispatch_task(recomp_func_t entry_function)
{
    static _Thread_local int stack_guard_enabled = -1;
    if (stack_guard_enabled < 0)
        stack_guard_enabled = getenv("MKDA_TASK_STACK_GUARD") != NULL;
    header_guard_enabled = stack_guard_enabled;
    unsigned char stack_guard[256];
    uint32_t stack_guard_start = 0, stack_guard_size = 0;
    uint32_t task = MEM32(0x343648);
    if (active) {
        fprintf(stderr, "[MKDA] Nested task dispatcher parent=%08X child=%08X"
                        " parent-entry=%08X child-entry=%08X return=%08X\n",
                active->task, task, MEM32(active->task + 0x30), MEM32(task + 0x30),
                MEM32(g_esp));
        abort();
    }
    TaskFiber *fiber;
    for (fiber = fibers; fiber && fiber->task != task; fiber = fiber->next) {}
    if (!fiber) {
        fiber = calloc(1, sizeof(*fiber));
        if (!fiber) abort();
        fiber->task = task;
        fiber->next = fibers; fibers = fiber;
        fprintf(stderr, "[MKDA] Native coroutine for task %08X entry %08X callback %08X\n",
                task, MEM32(task + 0x30), MEM32(task + 0x2c));
    }
    /* Guest task storage can be recycled or explicitly restarted. Only resume
     * native frames if the guest still describes the continuation we saved. */
    if (fiber->suspended &&
        (MEM32(task + 4) != fiber->task_id ||
         MEM32(task + 0x30) != fiber->suspended_entry ||
         MEM32(task + 0x34) != fiber->suspended_resume ||
         MEM32(task + 0x44) != fiber->suspended_sp ||
         MEM32(task + 0x40) != fiber->suspended_backing ||
         (MEM32(task + 0x40) && entry_function != sub_000E1800_gen &&
          MEM32(task + 0x44) >= MEM32(task + 0x40)))) {
        fprintf(stderr, "[MKDA] Restart recycled task %08X (id %08X -> %08X,"
                        " entry %08X -> %08X, resume %08X -> %08X, sp %08X -> %08X)\n",
                task, fiber->task_id, MEM32(task + 4),
                fiber->suspended_entry, MEM32(task + 0x30),
                fiber->suspended_resume, MEM32(task + 0x34),
                fiber->suspended_sp, MEM32(task + 0x44));
        fiber->suspended = 0;
        free(fiber->stack); fiber->stack = NULL;
        free(fiber->guest_stack_copy); fiber->guest_stack_copy = NULL;
        fiber->guest_stack_size = 0;
    }
    /* E1310 assigns a fresh decreasing ID at +4 for each task lifetime. */
    if (fiber->task_id != MEM32(task + 4)) fiber->stack_header = 0;
    fiber->task_id = MEM32(task + 4);
    /* Retain the pre-pop entry for the original new-task dispatcher. */
    GuestState entry;
    save_guest(&entry);
    MEM32(0x2f7174) = MEM32(g_esp);
    MEM32(0x2f7184) = g_esp + 4;
    MEM32(0x2f7194) = g_ebp;
    save_guest(&scheduler_guest);
    scheduler_guest.sp += 4;
    last_task = task;
    last_scheduler_sp = scheduler_guest.sp;
    last_scheduler_bp = scheduler_guest.bp;
    if (stack_guard_enabled && scheduler_guest.bp >= scheduler_guest.sp &&
        scheduler_guest.bp - scheduler_guest.sp <= sizeof(stack_guard) - 8) {
        stack_guard_start = scheduler_guest.sp;
        stack_guard_size = scheduler_guest.bp + 8 - stack_guard_start;
        memcpy(stack_guard, (void *)XBOX_PTR(stack_guard_start), stack_guard_size);
    }
    if (stack_guard_enabled) {
        if (stack_guard_size) ++guard_checks; else ++guard_skips;
    }
    if (!fiber->suspended) {
        fiber->entry_armed = 0;
        fiber->entry = entry_function;
        if (!fiber->stack) fiber->stack = malloc(2 * 1024 * 1024);
        if (!fiber->stack) abort();
        fiber->guest = entry;
        fiber->guest_stack_top = scheduler_guest.sp;
        if (mkda_context_init(&fiber->native, fiber->stack, 2 * 1024 * 1024,
                              task_start, &scheduler)) abort();
    }
    active = fiber;
    if (fiber->suspended) {
        if (fiber->guest_stack_size) {
            if (scheduler_guest.sp != fiber->guest_stack_top) {
                fprintf(stderr, "[MKDA] Shared task stack moved across dispatch\n"); abort();
            }
            memcpy((void *)XBOX_PTR(fiber->guest_stack_start), fiber->guest_stack_copy,
                   fiber->guest_stack_size);
        }
        restore_guest(&fiber->guest);
    }
    if (stack_guard_enabled && entry_function == sub_000E1B50_gen) {
        /* E14A0 allocates 4000 bytes; E10A0 frees top-4000. AED0 locates
         * its allocation header using the alignment byte at user_pointer-1. */
        uint32_t top = MEM32(task + 0x40);
        if (top >= 0x5000 && top < 0x4000000) {
            uint32_t bottom = top - 0x4000;
            if (!fiber->stack_header || fiber->stack_allocation_top != top) {
                fiber->stack_padding = MEM8(bottom - 1);
                fiber->stack_allocation_top = top;
                fiber->stack_header = bottom - 0x10 - fiber->stack_padding;
                fiber->stack_allocation_size = MEM32(fiber->stack_header + 8);
            } else {
                check_task_stack_header();
            }
        }
    } else {
        fiber->stack_header = 0;
    }
    if (mkda_context_swap(&scheduler, &fiber->native)) abort();
    check_task_stack_header();
    restore_guest(&scheduler_guest);
    if (stack_guard_size && memcmp(stack_guard, (void *)XBOX_PTR(stack_guard_start),
                                   stack_guard_size)) {
        fprintf(stderr, "[MKDA] Task %08X changed scheduler frame %08X..%08X\n",
                task, stack_guard_start, stack_guard_start + stack_guard_size);
        fprintf(stderr, "  entry=%08X callback=%08X resume=%08X flags=%08X backing=%08X saved-sp=%08X"
                        " suspended=%d dispatcher=%p\n",
                MEM32(task + 0x30), MEM32(task + 0x2c), MEM32(task + 0x34), MEM32(task + 0x20),
                MEM32(task + 0x40), MEM32(task + 0x44), fiber->suspended,
                (void *)entry_function);
        for (uint32_t i = 0; i + 4 <= stack_guard_size; i += 4) {
            uint32_t before;
            memcpy(&before, stack_guard + i, sizeof(before));
            uint32_t after = MEM32(stack_guard_start + i);
            if (before != after)
                fprintf(stderr, "  frame[%08X] %08X -> %08X\n", stack_guard_start + i,
                        before, after);
        }
        abort();
    }
    active = NULL;
    if (!fiber->suspended) {
        free(fiber->stack);
        fiber->stack = NULL;
        free(fiber->guest_stack_copy);
        fiber->guest_stack_copy = NULL;
        fiber->guest_stack_size = 0;
    }
}

void sub_000E1B50(void)
{
    dispatch_task(sub_000E1B50_gen);
}

void sub_000E1800(void)
{
    dispatch_task(sub_000E1800_gen);
}

void sub_000E18C0(void)
{
    dispatch_task(sub_000E18C0_gen);
}

static void yield_task(int shared_stack)
{
    if (!active) { fprintf(stderr, "[MKDA] Yield outside coroutine\n"); abort(); }
    check_task_stack_header();
    uint32_t task = MEM32(0x343648);
    MEM32(task + 0x10) = g_ebp; MEM32(task + 0x14) = g_ebx;
    MEM32(task + 0x18) = g_esi; MEM32(task + 0x1c) = g_edi;
    MEM32(0x2f7170) = MEM32(g_esp); g_esp += 4;
    MEM32(task + 0x44) = g_esp;
    MEM32(task + 0xc) = MEM32(0x343640);
    MEM32(task + 0x34) = MEM32(0x2f7170);
    if (shared_stack) {
        uint32_t top = MEM32(0x2f7184);
        if (top < g_esp || top - g_esp > 2 * 1024 * 1024) {
            fprintf(stderr, "[MKDA] Invalid shared task stack %08X..%08X\n", g_esp, top);
            abort();
        }
        uint32_t size = top - g_esp;
        /* The 28B670 constructor E13A0 uses E1400's 200-byte allocation. */
        if (size > 0x200 && MEM32(task) == 0x28b670 &&
            getenv("MKDA_TASK_STACK_GUARD")) {
            fprintf(stderr, "[MKDA] Shared task %08X exceeds 200-byte backing:"
                            " entry=%08X resume=%08X sp=%08X top=%08X size=%X\n",
                    task, MEM32(task + 0x30), MEM32(task + 0x34), g_esp, top, size);
            abort();
        }
        void *copy = realloc(active->guest_stack_copy, size ? size : 1);
        if (!copy) abort();
        active->guest_stack_copy = copy;
        active->guest_stack_start = g_esp;
        active->guest_stack_size = size;
        memcpy(copy, (void *)XBOX_PTR(g_esp), size);
        uint32_t stored_top = MEM32(task + 0x40);
        if (stored_top < size) abort();
        memmove((void *)XBOX_PTR(stored_top - size), copy, size);
        MEM32(task + 0x44) = stored_top - size;
        MEM32(0x2f7180) = g_esp;
        MEM32(0x2f7198) = g_esp;
        MEM32(0x2f7190) = stored_top - size;
    }
    /* The original yields save task registers before invoking this callback.
     * In particular its lifted EBP/SEH publication must not become the task's
     * resume frame. Keep memory side effects, but resume the saved registers. */
    save_guest(&active->guest);
    /* A callback can reset/repoint this task. Remember the continuation it
     * was given, so those changes restart it instead of resuming stale C. */
    active->suspended_entry = MEM32(task + 0x30);
    active->suspended_resume = MEM32(task + 0x34);
    active->suspended_sp = MEM32(task + 0x44);
    active->suspended_backing = MEM32(task + 0x40);
    {
        static _Thread_local int trace = -1;
        if (trace < 0) {
            const char *e = getenv("MKDA_TASK_TRACE");
            trace = e && *e == '1';
        }
        if (trace && active->trace_entry != active->suspended_entry) {
            active->trace_entry = active->suspended_entry;
            active->trace_count = 0;
        }
        if (trace && active->trace_count < 16) {
            uint32_t pc = active->suspended_resume, i;
            for (i = 0; i < active->trace_count; ++i)
                if (active->trace_resume[i] == pc) break;
            if (i == active->trace_count) {
                active->trace_resume[active->trace_count++] = pc;
                fprintf(stderr, "[TASK-TRACE] task %08X entry %08X resume %08X callback %08X"
                        " eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X\n",
                        task, active->suspended_entry, pc, MEM32(task + 0x2c),
                        g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp);
            }
        }
    }
    uint32_t callback = MEM32(task + 0x2c);
    if (callback) {
        recomp_func_t function = recomp_lookup_manual(callback);
        if (!function) function = recomp_lookup(callback);
        if (!function) { fprintf(stderr, "[MKDA] Missing yield callback %08X\n", callback); abort(); }
        PUSH32(g_esp, 0x000E1CCB);
        function();
    }
    check_task_stack_header();
    active->suspended = 1;
    restore_guest(&scheduler_guest);
    if (mkda_context_swap(&active->native, &scheduler)) abort();
    /* ucontext captured the scheduler's native FP environment on the way
     * out. Reapply the resumed task's SSE state after that native restore. */
    recomp_set_mxcsr(active->guest.mxcsr);
}

void sub_000E1C80(void)
{
    yield_task(0);
}

void sub_000E1A30(void)
{
    yield_task(1);
}

/* Paired helpers temporarily run a task's code on the scheduler guest stack.
 * Their dynamic JMP goes to the just-popped CALL return address; native C
 * already has that continuation, so only the guest ESP needs changing. */
void sub_000E1CF0(void)
{
    MEM32(0x2f7170) = MEM32(g_esp);
    MEM32(0x2f7188) = g_esp + 4;
    g_esp = MEM32(0x2f7184);
}

void sub_000E1D10(void)
{
    MEM32(0x2f7170) = MEM32(g_esp);
    g_esp = MEM32(0x2f7188);
    MEM32(0x2f7188) = 0;
}
