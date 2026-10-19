#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL.h>
#include "nv2a_gl.h"
#include "nv2a_backend.h"
#include "xbox_memory_layout.h"
typedef void (*recomp_func_t)(void);
extern int nv2a_renderer_replay(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; abort(); }
int main(int argc, char **argv)
{
    assert(setenv("RECOMP_GL_THREAD", argc > 1 ? argv[1] : "1", 1) == 0);
    assert(setenv("RECOMP_PROG_CACHE", "0", 1) == 0);
    if (argc > 3) {
        FILE *file = fopen(argv[3], "rb");
        assert(file && fseek(file, 0, SEEK_END) == 0);
        long size = ftell(file);
        assert(size > 0);
        rewind(file);
        void *xbe = malloc((size_t)size);
        assert(xbe && fread(xbe, 1, (size_t)size, file) == (size_t)size);
        fclose(file);
        assert(xbox_MemoryLayoutInit(xbe, (size_t)size));
        free(xbe);
    }
    assert(setenv("RECOMP_WATCHDOG_SECS", "60", 1) == 0);
    xbox_WatchdogStart();
    nv2a_gl_install();
    if (argc > 3) {
        volatile uint32_t *marker = (volatile uint32_t *)
            ((uint8_t *)xbox_GetMemoryOffset() + 0x006FF000);
        *marker = 0;
        for (uint32_t i = 1; i <= 256; ++i) {
            int queued = nv2a_queue_poke(0x006FF000, i);
            assert(queued == (argv[1][0] == '1'));
            if (!queued) *marker = i;
        }
        nv2a_queue_settle();
        assert(*marker == 256); /* Fence wake must observe completed writes. */
    }
    if (argc > 4) {
        assert(setenv("RECOMP_REPLAY", argv[4], 1) == 0);
        assert(nv2a_renderer_replay() == 0);
    }
    if (argc > 2) SDL_Delay(25); /* Shut down both before and after idle wait. */
    assert(xbox_MemoryLayoutStopWorkers(2000));
    assert(nv2a_gl_shutdown());
    assert(nv2a_gl_shutdown());
    xbox_MemoryLayoutShutdown();
    /* A cancelled watchdog can be started/stopped again without a sleeper. */
    xbox_WatchdogStart();
    assert(xbox_WatchdogStop(2000));
    SDL_Quit();
    return 0;
}
