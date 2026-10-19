#include <assert.h>
#include "recomp_types.h"

int main(void)
{
    uint32_t saved_control = recomp_get_mxcsr();
    recomp_set_mxcsr(0x1f80u);
    assert(recomp_get_mxcsr() == 0x1f80u);
    volatile float tiny_input = 0x1p-126f;
    __m128 tiny_vector = _mm_set_ss(tiny_input);
    __m128 half_vector = _mm_set_ss(0.5f);
    float gradual = _mm_cvtss_f32(_mm_mul_ss(tiny_vector, half_vector));
    assert(gradual != 0.0f);
    recomp_set_mxcsr(0x9f80u);
    tiny_vector = _mm_set_ss(tiny_input);
    float flushed = _mm_cvtss_f32(_mm_mul_ss(tiny_vector, half_vector));
    assert(flushed == 0.0f);
    assert(recomp_get_mxcsr() & 0x8000u);
    /* FTZ flushes results, but must retain a subnormal input that becomes
     * normal after multiplication. A global ARM FPCR.FZ setting loses it. */
    volatile uint32_t input_bits[4] = { 1u, 0x80000001u, 0x00800000u, 0x80800000u };
    RecompXmm input;
    for (int i = 0; i < 4; ++i) input.u[i] = input_bits[i];
    RecompXmm scale = { .f = { 0x1p126f, 0x1p126f, 0.5f, 0.5f } };
    RecompXmm result = XMM_MUL(input, scale);
    assert(result.u[0] == 0x34000000u); /* +2^-23, input was +2^-149 */
    assert(result.u[1] == 0xb4000000u);
    assert(result.u[2] == 0u);
    assert(result.u[3] == 0x80000000u); /* Preserve the sign of flushed zero. */
    recomp_set_mxcsr(saved_control);
    return 0;
}
