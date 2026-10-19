#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

_Thread_local uint32_t g_esp;
ptrdiff_t g_xbox_mem_offset;
void sub_0015AC20(void) {}
void sub_0015A880(void) {}
void sub_0015A990(void) {}
void mkda_heap_check_entry(void);
#define WORD(a) (*(uint32_t *)(g_xbox_mem_offset + (a)))

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 8 * 1024 * 1024);
    assert(g_xbox_mem_offset);
    g_esp = 0x8000;
    WORD(g_esp) = 0x15a811;
    WORD(g_esp + 4) = 0x1000;
    WORD(g_esp + 8) = 0x4000;
    WORD(0x100c) = 0x2000;
    WORD(0x2004) = 0x3000;
    mkda_heap_check_entry();
    assert(WORD(0x2004) == 0x3000);
    for (int test = 0; test < 6; ++test) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            if (test == 0) WORD(g_esp + 8) = 0x2000; /* double free */
            if (test == 1) WORD(0x3004) = 0x2000; /* cycle */
            if (test == 2) WORD(0x3004) = 0xf88304c4; /* invalid pointer */
            if (test >= 3) WORD(g_esp) = 0x15a7ef;
            if (test == 3) WORD(0x4000) = 0xf88304c4;
            if (test == 4) WORD(0x4004) = 0x7ffffffc;
            if (test == 5) WORD(0x4000) = 0x2000; /* nonreciprocal link */
            mkda_heap_check_entry();
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    return 0;
}
