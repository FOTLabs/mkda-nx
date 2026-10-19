/* Keep libnx's vocabulary separate from the runtime's Win32 types. */
#include <switch.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "switch_bootstrap.h"

extern ptrdiff_t g_xbox_mem_offset;
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *context)
{
    fprintf(stderr, "[MKDA] Exception %u pc=%016lX lr=%016lX sp=%016lX"
                    " far=%016lX guest=%08X\n",
            context->error_desc, (unsigned long)context->pc.x,
            (unsigned long)context->lr.x, (unsigned long)context->sp.x,
            (unsigned long)context->far.x,
            (uint32_t)(context->far.x - (uintptr_t)g_xbox_mem_offset));
    fprintf(stderr, " eax=%08X ebx=%08X ecx=%08X edx=%08X"
                    " esi=%08X edi=%08X esp=%08X\n",
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
    fflush(stderr);
    mkda_switch_flush_log();
    svcExitProcess();
}

/* libnx invokes the exception hook directly; POSIX signal unwinding and
 * its optional native-thread sampler are only available in the Linux build. */
void mkda_install_diagnostics(void) {}
void mkda_start_native_sampler(void) {}
