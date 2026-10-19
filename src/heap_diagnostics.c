/* Linux-only diagnostic build: observe the verified allocator calls.
 * Only their generated compilation unit gets function-entry instrumentation. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Thread_local uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void sub_0015AC20(void), sub_0015A880(void), sub_0015A990(void);

static uint32_t word(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (void *)(g_xbox_mem_offset + address), sizeof(value));
    return value;
}

static int valid(uint32_t address)
{
    return address >= 0x1000 && address < 0x4000000 - 0x40 && !(address & 3);
}

static _Thread_local struct { uint32_t heap, block, task; } history[32];
static _Thread_local unsigned frees;

static void fail(const char *reason, uint32_t heap, uint32_t block, uint32_t node)
{
    fprintf(stderr, "[HEAP] %s: heap=%08X block=%08X node=%08X sp=%08X\n",
            reason, heap, block, node, g_esp);
    for (unsigned i = frees > 32 ? frees - 32 : 0; i < frees; ++i)
        fprintf(stderr, "  free[%u] heap=%08X block=%08X task=%08X\n", i,
                history[i & 31].heap, history[i & 31].block, history[i & 31].task);
    if (valid(block))
        for (unsigned i = 0; i < 0x30; i += 4)
            fprintf(stderr, "  block+%02X=%08X\n", i, word(block + i));
    uint32_t task = word(0x343648);
    if (valid(task))
        for (unsigned i = 0; i < 0x48; i += 4)
            fprintf(stderr, "  task[%08X]+%02X=%08X\n", task, i, word(task + i));
    abort();
}

void mkda_heap_check_entry(void)
{
    uint32_t return_pc = valid(g_esp) ? word(g_esp) : 0;
    if (return_pc == 0x15a811 || return_pc == 0x15a7ef || return_pc == 0x15a801) {
        uint32_t heap = word(g_esp + 4), block = word(g_esp + 8);
        history[frees & 31].heap = heap;
        history[frees & 31].block = block;
        history[frees & 31].task = word(0x343648);
        ++frees;
        if (!valid(heap) || !valid(block)) fail("invalid arguments", heap, block, 0);
        if (return_pc != 0x15a811) {
            uint32_t before = word(block), after = word(block + 4);
            if (before && (!valid(before) || word(before + 4) != block))
                fail("invalid allocated predecessor", heap, block, before);
            if (after && (!valid(after) || word(after) != block))
                fail("invalid allocated successor", heap, block, after);
        }
        uint32_t node = word(heap + 0xc), previous = 0;
        unsigned count = 0;
        while (node) {
            if (!valid(node)) fail("invalid list pointer", heap, block, node);
            if (node == block) fail("block already free", heap, block, node);
            if (return_pc == 0x15a811 && previous && node <= previous)
                fail("unordered or cyclic list", heap, block, node);
            if (++count > 65536) fail("list too long", heap, block, node);
            previous = node;
            node = word(node + 4);
        }
    }
}

void __cyg_profile_func_enter(void *function, void *caller)
{
    (void)caller;
    if (function == (void *)sub_0015AC20 || function == (void *)sub_0015A880 ||
        function == (void *)sub_0015A990)
        mkda_heap_check_entry();
}

void __cyg_profile_func_exit(void *function, void *caller)
{
    (void)function; (void)caller;
}
