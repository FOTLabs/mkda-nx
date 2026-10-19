/* SSE1 arithmetic with a separate guest MXCSR. Never leave FPCR changed:
 * lifted x87 and host libraries share this thread's native FP environment. */
#ifdef __aarch64__
#include <stdint.h>
#include <string.h>

extern void recomp_unimpl(const char *, uint32_t);
static _Thread_local uint32_t mxcsr = 0x1f80;

uint32_t recomp_arm_get_mxcsr(void) { return mxcsr; }
void recomp_arm_set_mxcsr(uint32_t value)
{
    /* Masked SSE exceptions are implemented. Unmasked traps and DAZ need
     * separate handling; fail explicitly rather than changing their meaning. */
    if ((value & 0xffff0000u) || (value & 0x1fc0u) != 0x1f80u) {
        recomp_unimpl("ARM MXCSR: reserved bits, DAZ or unmasked exceptions", value);
        return;
    }
    mxcsr = value;
}

static uint32_t bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static float number(uint32_t u) { float v; memcpy(&v, &u, 4); return v; }
static int subnormal(uint32_t u) { return (u & 0x7fffffffu) && !(u & 0x7f800000u); }
static int nan_bits(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }

typedef struct { uint64_t control, status; } HostFp;
static HostFp begin(int approximate)
{
    HostFp old;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(old.control), "=r"(old.status));
    unsigned rc = approximate ? 0 : (mxcsr >> 13) & 3;
    /* x86: nearest/down/up/zero; ARM: nearest/up/down/zero. */
    unsigned arm_rc = ((rc & 1) << 1) | ((rc & 2) >> 1);
    uint64_t control = (uint64_t)arm_rc << 22;
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, xzr" :: "r"(control) : "memory");
    return old;
}
static uint32_t end(HostFp old)
{
    uint64_t status;
    __asm__ volatile("mrs %0, fpsr\n\tmsr fpcr, %1\n\tmsr fpsr, %2"
                     : "=&r"(status) : "r"(old.control), "r"(old.status) : "memory");
    return (uint32_t)((status & 1) | ((status & 0x1e) << 1));
}

/* SSE tests tininess after rounding to its normal precision with an
 * unbounded exponent, before denormal rounding. ARM tests before rounding.
 * Only the normal/subnormal boundary needs this rare scaled recomputation. */
static int tiny_at_boundary(float a, float b, int operation)
{
    HostFp old = begin(0);
    float scale = 0x1p24f, scaled_a, scaled_b = b, result;
    __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(scaled_a) : "w"(a), "w"(scale));
    if (operation < 2)
        __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(scaled_b) : "w"(b), "w"(scale));
    switch (operation) {
    case 0: __asm__ volatile("fadd %s0, %s1, %s2" : "=w"(result) : "w"(scaled_a), "w"(scaled_b)); break;
    case 1: __asm__ volatile("fsub %s0, %s1, %s2" : "=w"(result) : "w"(scaled_a), "w"(scaled_b)); break;
    case 2: __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(result) : "w"(scaled_a), "w"(b)); break;
    case 3: __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(result) : "w"(scaled_a), "w"(b)); break;
    default: result = 0x1p-102f; break;
    }
    (void)end(old);
    return (bits(result) & 0x7fffffffu) < 0x0c800000u; /* 2^-126 * 2^24 */
}

/* 0 add, 1 subtract, 2 multiply, 3 divide, 4 sqrt, 5 rsqrt, 6 reciprocal. */
float recomp_arm_sse_f32(float a, float b, int operation)
{
    if (operation < 0 || operation > 6) {
        recomp_unimpl("ARM SSE operation", (uint32_t)operation); return 0;
    }
    uint32_t ua = bits(a), ub = bits(b), result_bits, flags;
    float result;
    int approximate = operation >= 5;
    if (approximate) {
        /* RCP/RSQRT ignore RC, treat denormal inputs as signed zero, and
         * never change exception flags. Their precision is approximate. */
        if (nan_bits(ua)) return number(ua | 0x00400000u);
        if (subnormal(ua)) a = number(ua & 0x80000000u);
        ua = bits(a);
        if (operation == 5 && (ua & 0x80000000u) && (ua & 0x7fffffffu))
            return number(0xffc00000u);
    }
    HostFp old = begin(approximate);
    switch (operation) {
    case 0: __asm__ volatile("fadd %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b)); break;
    case 1: __asm__ volatile("fsub %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b)); break;
    case 2: __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b)); break;
    case 3: __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b)); break;
    case 4: __asm__ volatile("fsqrt %s0, %s1" : "=w"(result) : "w"(a)); break;
    case 5: {
        float root, one = 1.0f;
        __asm__ volatile("fsqrt %s0, %s1" : "=w"(root) : "w"(a));
        __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(result) : "w"(one), "w"(root));
        break;
    }
    case 6: {
        float one = 1.0f;
        __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(result) : "w"(one), "w"(a));
        break;
    }
    default: result = 0; break;
    }
    flags = end(old);
    result_bits = bits(result);
    if (approximate) {
        if (subnormal(result_bits)) result_bits &= 0x80000000u;
        return number(result_bits);
    }
    /* SSE propagates the first NaN operand, quieting it; ARM can prefer a
     * signaling second operand. Invalid non-NaN operations use indefinite. */
    if (nan_bits(ua)) result_bits = ua | 0x00400000u;
    else if (operation < 4 && nan_bits(ub)) result_bits = ub | 0x00400000u;
    else if (flags & 1) result_bits = 0xffc00000u;
    if ((flags & 0x10) && (result_bits & 0x7fffffffu) == 0x00800000u &&
        !tiny_at_boundary(a, b, operation)) flags &= ~0x10u;
    if (!(flags & 4) && !nan_bits(result_bits) &&
        (subnormal(ua) || (operation < 4 && subnormal(ub))))
        flags |= 2;
    if ((mxcsr & 0x8000) && (subnormal(result_bits) || (flags & 0x10))) {
        result_bits &= 0x80000000u;
        flags |= 0x30;
    }
    mxcsr |= flags;
    return number(result_bits);
}

int32_t recomp_arm_sse_to_i32(float value, int truncate)
{
    HostFp old = begin(0);
    int32_t result;
    if (truncate) {
        __asm__ volatile("fcvtzs %w0, %s1" : "=r"(result) : "w"(value));
    } else {
        float rounded;
        __asm__ volatile("frintx %s0, %s1" : "=w"(rounded) : "w"(value));
        __asm__ volatile("fcvtzs %w0, %s1" : "=r"(result) : "w"(rounded));
    }
    uint32_t flags = end(old);
    if (flags & 1) result = (int32_t)0x80000000u;
    /* CVT's denormal input response is precision, not the arithmetic DE. */
    mxcsr |= flags;
    return result;
}

float recomp_arm_sse_from_i32(int32_t value)
{
    HostFp old = begin(0);
    float result;
    __asm__ volatile("scvtf %s0, %w1" : "=w"(result) : "r"(value));
    mxcsr |= end(old);
    return result;
}
#endif
