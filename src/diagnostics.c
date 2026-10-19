#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <execinfo.h>
#include <pthread.h>
#include <ucontext.h>
#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS int g_guest_interrupts_enabled;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_gil_debug_dump(void);
extern void mkda_task_debug_dump(void);
static unsigned char alternate_stack[65536];
static pthread_t sampled_thread;
static unsigned sample_delay;

static void sample_report(int signal_number, siginfo_t *info, void *native_context)
{
    (void)signal_number;
    (void)info;
    void *frames[48];
    fprintf(stderr, "[MKDA] Native sample esp=%08X eax=%08X ecx=%08X IF=%d\n",
            g_esp, g_eax, g_ecx, g_guest_interrupts_enabled);
    xbox_gil_debug_dump();
    mkda_task_debug_dump();
#if defined(__x86_64__)
    ucontext_t *context = native_context;
    uint64_t frame = context->uc_mcontext.gregs[REG_R14];
    fprintf(stderr, "  [HOST] rip=%016llX rbp=%016llX r14=%016llX\n",
            (unsigned long long)context->uc_mcontext.gregs[REG_RIP],
            (unsigned long long)context->uc_mcontext.gregs[REG_RBP],
            (unsigned long long)frame);
    /* In the sampled scheduler build r14 carries its guest EBP. Show this
     * candidate only when it lies in RAM; other functions use r14 differently. */
    if (frame >= 0x1000 && frame < 0x4000000 && !(frame & 3)) {
        for (uint32_t a = (uint32_t)frame - 24; a <= frame + 8; a += 4)
            fprintf(stderr, "  candidate-frame[%08X]=%08X\n", a,
                    *(uint32_t *)((uintptr_t)g_xbox_mem_offset + a));
    }
#else
    (void)native_context;
#endif
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

static void *sample_later(void *unused)
{
    (void)unused;
    sleep(sample_delay);
    pthread_kill(sampled_thread, SIGUSR1);
    return NULL;
}

void mkda_start_native_sampler(void)
{
    const char *setting = getenv("MKDA_SAMPLE_SECS");
    if (!setting) return;
    int delay = atoi(setting);
    if (delay < 1 || delay > 600) return;
    sample_delay = (unsigned)delay;
    sampled_thread = pthread_self();
    void *warmup[4];
    backtrace(warmup, 4);
    struct sigaction action = {0};
    action.sa_sigaction = sample_report;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    pthread_t sampler;
    if (!pthread_create(&sampler, NULL, sample_later, NULL)) pthread_detach(sampler);
}

static void crash_report(int signal_number, siginfo_t *info, void *context)
{
    (void)context;
    void *frames[40];
    fprintf(stderr, "[MKDA] Signal %d at host %p\n", signal_number, info->si_addr);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "  esi=%08X edi=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    if (g_xbox_mem_offset && g_esp >= XBOX_STACK_BASE && g_esp < XBOX_STACK_TOP) {
        uint32_t end = g_esp + 1024;
        if (end > XBOX_STACK_TOP) end = XBOX_STACK_TOP;
        for (uint32_t address = g_esp; address < end; address += 4) {
            uint32_t value = *(uint32_t *)((uintptr_t)g_xbox_mem_offset + address);
            if (value >= g_xbox_code_lo && value < g_xbox_code_hi)
                fprintf(stderr, "  stack[%08X]=%08X\n", address, value);
        }
    }
    int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + signal_number);
}

void mkda_install_diagnostics(void)
{
    stack_t stack = {.ss_sp = alternate_stack, .ss_size = sizeof(alternate_stack)};
    sigaltstack(&stack, NULL);
    struct sigaction action = {0};
    action.sa_sigaction = crash_report;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
}
