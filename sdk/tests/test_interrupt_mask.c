#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t address) { (void)address; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
extern void xbox_gil_enter(void), xbox_gil_leave(void), recomp_preempt(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth), recomp_set_interrupts_enabled(int enabled);
extern volatile int g_gil_contended;
extern RECOMP_TLS int g_guest_interrupts_enabled;
static atomic_int worker_entered;

static void *worker(void *unused)
{
    (void)unused;
    assert(g_guest_interrupts_enabled == 1); /* per host thread */
    xbox_gil_enter();
    atomic_store(&worker_entered, 1);
    xbox_gil_leave();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    xbox_gil_enter();
    recomp_set_interrupts_enabled(0);
    int result = pthread_create(&thread, NULL, worker, NULL);
    assert(result == 0);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!g_gil_contended) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - start.tv_sec < 3);
    }
    recomp_preempt();
    assert(!atomic_load(&worker_entered));
    int depth = xbox_gil_suspend();
    assert(depth == 0 && !atomic_load(&worker_entered));
    xbox_gil_resume(depth);
    recomp_set_interrupts_enabled(1);
    recomp_preempt();
    assert(atomic_load(&worker_entered));
    xbox_gil_leave();
    result = pthread_join(thread, NULL);
    assert(result == 0);
    return 0;
}
