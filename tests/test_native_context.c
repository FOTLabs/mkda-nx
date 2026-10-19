#define _XOPEN_SOURCE 700
#include "../src/native_context.h"
#include <assert.h>
#include <fenv.h>
#include <stdlib.h>

static NativeContext root, tasks[2];
static int progress[2];

static void run(unsigned index)
{
    volatile unsigned local[64];
    for (unsigned i = 0; i < 64; ++i) local[i] = index * 100 + i;
    int rounding = index ? FE_UPWARD : FE_DOWNWARD;
    assert(fesetround(rounding) == 0);
    assert(feclearexcept(FE_ALL_EXCEPT) == 0);
    assert(feraiseexcept(FE_DIVBYZERO) == 0);
    for (int cycle = 0; cycle < 100; ++cycle) {
        ++progress[index];
        assert(mkda_context_swap(&tasks[index], &root) == 0);
        for (unsigned i = 0; i < 64; ++i) assert(local[i] == index * 100 + i);
        assert(fegetround() == rounding);
        assert(fetestexcept(FE_DIVBYZERO));
    }
    ++progress[index];
    mkda_context_jump(&root);
    abort();
}
static void first(void) { run(0); }
static void second(void) { run(1); }

int main(void)
{
    void *stacks[2] = {malloc(65536), malloc(65536)};
    assert(stacks[0] && stacks[1]);
    assert(fesetround(FE_TONEAREST) == 0);
    assert(feclearexcept(FE_ALL_EXCEPT) == 0);
    assert(mkda_context_init(&tasks[0], stacks[0], 65536, first, &root) == 0);
    assert(mkda_context_init(&tasks[1], stacks[1], 65536, second, &root) == 0);
    for (int cycle = 0; cycle <= 100; ++cycle) {
        for (unsigned i = 0; i < 2; ++i) {
            assert(mkda_context_swap(&root, &tasks[i]) == 0);
            assert(progress[i] == cycle + 1);
            assert(fegetround() == FE_TONEAREST);
            assert(!fetestexcept(FE_DIVBYZERO));
        }
    }
    free(stacks[0]); free(stacks[1]);
    return 0;
}
