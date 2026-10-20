#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "recomp_funcs.h"

extern void sub_00058E80(void);
extern int mkda_frame_paced_context(uint32_t);
extern uint64_t mkda_frame_wait(uint64_t *, uint64_t, uint64_t, int);
static int enabled, selected, entered;
static uint64_t deadline, started, ticks;

static uint64_t clock_ns(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

/* Explicit Linux diagnostic: use the retail ending entry in place of the
 * first attract task, after normal device/renderer/scheduler initialization.
 * No controller replay, unlock modification, or Switch release code. */
static void preview_start(void)
{
    MEM32(0x269cb4) = 21; /* verified character table: KANO */
    fprintf(stderr, "[ENDING-PREVIEW] Starting Kano through retail58E80\n");
    sub_00058E80();
}

recomp_func_t mkda_ending_preview_lookup(uint32_t va)
{
    if (va != 0x23970 || selected) return NULL;
    const char *value = getenv("MKDA_ENDING_PREVIEW");
    if (!value || strcmp(value, "kano")) return NULL;
    selected = enabled = 1;
    return preview_start;
}

void mkda_ending_preview_frame(uint32_t context)
{
    if (!enabled) return;
    int paced = !getenv("MKDA_ENDING_PREVIEW_UNPACED") && mkda_frame_paced_context(context);
    uint64_t wait = mkda_frame_wait(&deadline, clock_ns(), 1000000000, paced);
    if (wait) {
        struct timespec time = { (time_t)(wait / 1000000000), (long)(wait % 1000000000) };
        while (nanosleep(&time, &time) && errno == EINTR) { }
    }
    if (context == 14) {
        if (!entered) { entered = 1; started = clock_ns(); }
        ++ticks;
        if (ticks % 300 == 0)
            fprintf(stderr, "[ENDING-PREVIEW] ticks=%llu elapsed=%.3f context=14\n",
                    (unsigned long long)ticks, (clock_ns() - started) / 1e9);
    } else if (entered) {
        fprintf(stderr, "[ENDING-PREVIEW] Complete ticks=%llu elapsed=%.3f next-context=%u\n",
                (unsigned long long)ticks, (clock_ns() - started) / 1e9, context);
        fflush(NULL);
        _Exit(0); /* bounded diagnostic; production launch never takes this path */
    }
}
