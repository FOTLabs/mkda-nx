#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"

void recomp_unimpl(const char *text, uint32_t va)
{
    fprintf(stderr, "%s: %08X\n", text, va); abort();
}
static uint32_t bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static float number(uint32_t u) { float v; memcpy(&v, &u, 4); return v; }
static __attribute__((noinline)) uint32_t calculate(uint32_t a, uint32_t b, unsigned op)
{
    float x = number(a), y = number(b);
    switch (op) {
    case 0: return bits(recomp_sse_add32(x, y));
    case 1: return bits(recomp_sse_sub32(x, y));
    case 2: return bits(recomp_sse_mul32(x, y));
    case 3: return bits(recomp_sse_div32(x, y));
    case 4: return bits(recomp_sse_sqrt32(x));
    case 5: return bits(recomp_sse_rsqrt32(x));
    case 6: return bits(recomp_sse_rcp32(x));
    case 7: return (uint32_t)recomp_sse_to_i32(x, 1);
    case 8: return (uint32_t)recomp_sse_to_i32(x, 0);
    case 9: return bits(recomp_sse_from_i32((int32_t)a));
    default: abort();
    }
}
int main(void)
{
    static const uint32_t anchors[] = {
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
        0x00800000, 0x80800000, 0x00800001, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7fc12345, 0xffc23456, 0x7f812345, 0xff823456,
        0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x3f800001,
        0x3fc00000, 0xbfc00000, 0x40000000, 0x40400000,
        0x33800000, 0x4b800000, 0x4b800001, 0x4effffff, 0x4f000000, 0xcf000000,
    };
    uint32_t values[96], random = 0x749321ab;
    memcpy(values, anchors, sizeof anchors);
    for (unsigned i = sizeof anchors / sizeof *anchors; i < 96; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        values[i] = random;
    }
    uint32_t saved = recomp_get_mxcsr();
#ifdef __aarch64__
    uint64_t host_control = (1u << 24) | (2u << 22), host_status = 0x08000013;
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1" :: "r"(host_control), "r"(host_status) : "memory");
#endif
    for (unsigned mode = 0; mode < 8; ++mode) {
        uint32_t control = 0x1f80 | ((mode & 3) << 13) | ((mode >> 2) << 15);
        for (unsigned op = 0; op < 10; ++op) {
            for (unsigned a = 0; a < sizeof values / sizeof *values; ++a) {
                unsigned count = op < 4 ? sizeof values / sizeof *values : 1;
                for (unsigned b = 0; b < count; ++b) {
                    recomp_set_mxcsr(control);
                    uint32_t result = calculate(values[a], values[b], op);
                    uint32_t status = recomp_get_mxcsr();
#ifdef __aarch64__
                    uint64_t actual_control, actual_status;
                    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(actual_control), "=r"(actual_status));
                    assert(actual_control == host_control && actual_status == host_status);
#endif
                    printf("%08X %u %08X %08X %08X %08X\n", control, op, values[a], values[b], result, status);
                }
            }
        }
    }
    recomp_set_mxcsr(0x1fbf);
    (void)calculate(0x3f800000, 0x3f800000, 0);
    assert(recomp_get_mxcsr() == 0x1fbf); /* Sticky flags survive exact arithmetic. */
    RecompXmm packed_input = { .u = { 1, 0x80000001, 0x00800000, 0x80800000 } };
    RecompXmm packed_scale = { .f = { 0x1p126f, 0x1p126f, 0.5f, 0.5f } };
    recomp_set_mxcsr(0x9f80);
    RecompXmm packed_result = XMM_MUL(packed_input, packed_scale);
    assert(packed_result.u[0] == 0x34000000 && packed_result.u[1] == 0xb4000000);
    assert(packed_result.u[2] == 0 && packed_result.u[3] == 0x80000000);
    assert(recomp_get_mxcsr() == 0x9fb2);
    recomp_set_mxcsr(saved);
    return 0;
}
