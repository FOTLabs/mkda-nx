#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_funcs.h"

typedef void (*recomp_func_t)(void);
extern void mkda_task_complete(void);
extern void mkda_task_replace(void);
extern void mkda_task_replace_private(void);
extern recomp_func_t mkda_task_lookup_entry(uint32_t va);
extern void sub_00024FB0(void);
extern void sub_002075C9(void);
static uint32_t movie_decoder;

static int dash_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("MKDA_DASH_TRACE");
        enabled = e && *e == '1';
    }
    return enabled && MEM32(0x44cae0) == 2;
}

/* Observational probes around the original input, animation and lesson code.
 * Keep the lifted bodies and their guest calling conventions unchanged. */
extern void sub_00144D30_gen(void);
void sub_00144D30(void)
{
#if defined(__SWITCH__)
    /* CB740 advances 44CACC once before this poll. Direction handler128910
     * compares successive taps against13 ticks; running the loop uncapped
     * shrinks that interval with renderer speed. Arcade endings (context14)
     * likewise use frame-counted scene/subtitle waits alongside real audio. */
    extern void mkda_game_frame_pace(int active);
    extern int mkda_frame_paced_context(uint32_t context);
    mkda_game_frame_pace(mkda_frame_paced_context(MEM32(0x2f6e64)));
#elif !defined(_WIN32)
    extern void mkda_ending_preview_frame(uint32_t) __attribute__((weak));
    if (mkda_ending_preview_frame) mkda_ending_preview_frame(MEM32(0x2f6e64));
#endif
    sub_00144D30_gen();
    static uint32_t previous[6];
    static unsigned count;
    uint32_t now[6];
    for (unsigned i = 0; i < 6; ++i) now[i] = MEM32(0x2f0988 + 4 * i);
    if (dash_trace_enabled() && count < 256 &&
        memcmp(previous, now, sizeof(now)) != 0) {
        memcpy(previous, now, sizeof(now));
        ++count;
        fprintf(stderr, "[DASH-INPUT] tick %u mode %u p0 %08X/%08X/%08X p1 %08X/%08X/%08X\n",
                MEM32(0x44cacc), MEM32(0x2f6e64),
                now[0], now[1], now[2], now[3], now[4], now[5]);
        for (unsigned player = 0; player < 2; ++player) {
            uint32_t fighter = MEM32(0x30c760 + player * 4);
            if (fighter && fighter < 0x3fff000)
                fprintf(stderr, "[DASH-STATE] p%u fighter %08X state %08X task %08X anim %08X flags %08X time %08X end %08X\n",
                        player, fighter, MEM32(fighter + 0x194), MEM32(fighter + 0x10),
                        MEM32(fighter + 0xd38), MEM32(fighter + 0xd3c),
                        MEM32(fighter + 0xd44), MEM32(fighter + 0xd48));
            uint32_t object = MEM32(0x30c758 + player * 4);
            if (object && object < 0x3fff000)
                fprintf(stderr, "[DASH-POS] p%u object %08X flags %08X x %08X z %08X facing %08X\n",
                        player, object, MEM32(object + 8), MEM32(object + 0x84),
                        MEM32(object + 0x8c), MEM32(object + 0xa8));
        }
    }
}

extern void sub_00056420_gen(void);
void sub_00056420(void)
{
    static unsigned count;
    if (dash_trace_enabled() && count++ < 128) {
        uint32_t fighter = MEM32(0x342c38);
        fprintf(stderr, "[DASH-ANIM] tick %u fighter %08X state %08X anim %08X flags %08X blend %08X caller %08X\n",
                MEM32(0x44cacc), fighter, fighter ? MEM32(fighter + 0x194) : 0,
                MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp));
    }
    sub_00056420_gen();
}

extern void sub_000BD1B0_gen(void);
void sub_000BD1B0(void)
{
    static unsigned count;
    uint32_t event = MEM32(g_esp + 4);
    int trace = dash_trace_enabled() && count++ < 128;
    uint32_t counter = 0x2f68a8 + event * 20;
    if (trace)
        fprintf(stderr, "[DASH-EVENT] tick %u event %u side %u caller %08X active %u lesson-mode %u required %u credited %u\n",
                MEM32(0x44cacc), event, MEM32(g_esp + 8), MEM32(g_esp),
                MEM32(0x44e984), MEM32(0x2f6b74),
                event < 34 ? MEM32(counter + 8) : 0,
                event < 34 ? MEM32(counter + 12) : 0);
    sub_000BD1B0_gen();
    if (trace && event < 34)
        fprintf(stderr, "[DASH-CREDIT] event %u credited %u\n", event, MEM32(counter + 12));
}

/* XMV's audio callback rebases its presentation clock after packet stalls.
 * Verified at 2075C9: decoder+C4 = elapsed clock - completed packet time.
 * GetNextFrame returns that rebase plus content time (decoder+B8). MKDA's
 * intro captions compare the returned value to content-time script cues. */
static void movie_audio_clock(void)
{
    movie_decoder = MEM32(g_esp + 4);
    sub_002075C9();
}

static void movie_subtitle(void)
{
    static int trace = -1;
    static unsigned frames;
    if (trace < 0) {
        const char *e = getenv("RECOMP_AV_TIMING");
        trace = e && *e == '1';
    }
    uint32_t stack = g_esp;
    uint32_t timestamp = MEM32(stack + 4);
    uint32_t content = timestamp;
    if (movie_decoder) {
        uint32_t frame = MEM32(movie_decoder + 0xB8);
        uint32_t rebase = MEM32(movie_decoder + 0xC4);
        /* Match the exact frame to reject a stale decoder during transitions.
         * Unsigned addition also handles a negative clock rebase. */
        if (timestamp == frame + rebase) content = frame;
    }
    MEM32(stack + 4) = content;
    sub_00024FB0();
    MEM32(stack + 4) = timestamp;
    if (trace && frames++ % 30 == 0)
        fprintf(stderr, "[AV-SUBTITLE] frame %u timestamp %u content %u window %u..%u text %08X\n",
                frames, timestamp, content, MEM32(0x002E7F30), MEM32(0x002E7F2C), MEM32(0x002E7F28));
}

/* Add title-specific replacements only after examining MKDA's guest ABI. */
recomp_func_t recomp_lookup_manual(uint32_t va)
{
#if !defined(__SWITCH__) && !defined(_WIN32)
    extern recomp_func_t mkda_ending_preview_lookup(uint32_t) __attribute__((weak));
    if (mkda_ending_preview_lookup) {
        recomp_func_t preview = mkda_ending_preview_lookup(va);
        if (preview) return preview;
    }
#endif
    if (va == 0x00024FB0) return movie_subtitle;
    if (va == 0x002075C9) return movie_audio_clock;
    if (va == 0x000E1765) return mkda_task_complete;
    if (va == 0x000E1B10) return mkda_task_replace;
    if (va == 0x000E1D30) return mkda_task_replace_private;
    recomp_func_t entry = mkda_task_lookup_entry(va);
    if (entry) return entry;
    return NULL;
}

void recomp_icall_fail_log(uint32_t va)
{
    static _Thread_local uint32_t reported[64];
    static _Thread_local unsigned count;
    for (unsigned index = 0; index < count; ++index)
        if (reported[index] == va) return;
    if (count == 64) return;
    reported[count++] = va;
    fprintf(stderr, "[MKDA] Failed to resolve VA 0x%08X\n", va);
}

void recomp_icall_not_code_log(uint32_t va)
{
    fprintf(stderr, "[MKDA] Indirect call outside executable sections: %08X\n", va);
}

void recomp_unimpl(const char *text, uint32_t va)
{
    fprintf(stderr, "[MKDA] Unsupported instruction at %08X: %s\n", va, text);
    abort();
}
