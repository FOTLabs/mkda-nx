#include <string.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "recomp_funcs.h"

/* Audited XMV 207C4B: seven stdcall arguments: macroblock width/height,
 * planar Y/U/V, packed YUY2 destination and destination pitch. The original
 * interleaves 16x16 tiles with MMX, reusing each chroma row for two Y rows.
 * Retain the lifted body for comparison and explicit fallback. */
extern void sub_00207C4B_gen(void);
void sub_00207C4B(void)
{
    static int fallback = -1;
    if (fallback < 0) {
        const char *e = getenv("RECOMP_XMV_NATIVE_BLIT");
        fallback = e && *e == '0';
    }
    if (fallback) { sub_00207C4B_gen(); return; }
    RECOMP_PREEMPT();
    uint32_t width = MEM32(g_esp + 4) * 16;
    uint32_t height = MEM32(g_esp + 8) * 16;
    const uint8_t *y = (const uint8_t *)XBOX_PTR(MEM32(g_esp + 12));
    const uint8_t *u = (const uint8_t *)XBOX_PTR(MEM32(g_esp + 16));
    const uint8_t *v = (const uint8_t *)XBOX_PTR(MEM32(g_esp + 20));
    uint8_t *dst = (uint8_t *)XBOX_PTR(MEM32(g_esp + 24));
    uint32_t pitch = MEM32(g_esp + 28);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp = y + (size_t)row * width;
        const uint8_t *up = u + (size_t)(row / 2) * (width / 2);
        const uint8_t *vp = v + (size_t)(row / 2) * (width / 2);
        uint8_t *out = dst + (size_t)row * pitch;
#if defined(__aarch64__)
        for (uint32_t x = 0; x < width; x += 16) {
            uint8x8x2_t chroma = vzip_u8(vld1_u8(up + x/2), vld1_u8(vp + x/2));
            uint8x16x2_t packed = {{ vld1q_u8(yp + x),
                vcombine_u8(chroma.val[0], chroma.val[1]) }};
            vst2q_u8(out + 2*x, packed);
        }
#else
        for (uint32_t x = 0; x < width; x += 2) {
            out[2*x] = yp[x]; out[2*x+1] = up[x/2];
            out[2*x+2] = yp[x+1]; out[2*x+3] = vp[x/2];
        }
#endif
    }
    g_esp += 32;
}

/* Verified cdecl (dst, src, count), including backward overlap handling. */
void sub_001CC220(void)
{
    uint32_t destination = MEM32(g_esp + 4);
    uint32_t source = MEM32(g_esp + 8);
    uint32_t count = MEM32(g_esp + 12);
    if (count) memmove((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source), count);
    g_eax = destination;
    g_esp += 4;
}
