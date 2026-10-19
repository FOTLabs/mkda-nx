#ifndef MKDA_NATIVE_CONTEXT_H
#define MKDA_NATIVE_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Horizon has no ucontext. This backend switches at C call boundaries and
 * preserves AAPCS64 callee-saved registers plus floating-point control/status.
 * It never changes the thread pointer: all fibers stay on their owner thread. */
#if defined(__SWITCH__) || defined(MKDA_AARCH64_CONTEXT)
#if !defined(__aarch64__)
#error "The native Switch context backend requires AArch64"
#endif
#include <string.h>
typedef struct NativeContext {
    uint64_t registers[12]; /* x19..x30 */
    uint64_t sp;
    uint64_t doubles[8]; /* d8..d15 */
    uint64_t fpcr, fpsr;
} NativeContext;
_Static_assert(offsetof(NativeContext, sp) == 96, "context assembly layout");
_Static_assert(offsetof(NativeContext, doubles) == 104, "context assembly layout");
_Static_assert(offsetof(NativeContext, fpcr) == 168, "context assembly layout");
_Static_assert(offsetof(NativeContext, fpsr) == 176, "context assembly layout");
int mkda_context_swap(NativeContext *save, const NativeContext *load);
int mkda_context_jump(const NativeContext *load);
void mkda_context_entry(void);
static inline int mkda_context_init(NativeContext *context, void *stack,
                                    size_t size, void (*entry)(void),
                                    NativeContext *link)
{
    (void)link; /* task_start explicitly completes back to the scheduler */
    memset(context, 0, sizeof(*context));
    context->sp = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    context->registers[0] = (uintptr_t)entry;
    context->registers[11] = (uintptr_t)mkda_context_entry;
    __asm__ volatile("mrs %0, fpcr" : "=r"(context->fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(context->fpsr));
    return 0;
}
#else
#include <ucontext.h>
typedef ucontext_t NativeContext;
static inline int mkda_context_init(NativeContext *context, void *stack,
                                    size_t size, void (*entry)(void),
                                    NativeContext *link)
{
    if (getcontext(context)) return -1;
    context->uc_stack.ss_sp = stack;
    context->uc_stack.ss_size = size;
    context->uc_link = link;
    makecontext(context, entry, 0);
    return 0;
}
#define mkda_context_swap swapcontext
#define mkda_context_jump setcontext
#endif
#endif
