/*
 * gl_vsh.c -- NV2A vertex microcode to GLSL.
 *
 * The translation mirrors the executor's CPU interpreter (vp_run in
 * nv2a_pb_exec.c) instruction for instruction: the same field decode (the
 * 128-bit layout documented by xemu's vsh.c), the same parallel issue -- MAC
 * and ILU read every source before either writes -- and the same output
 * registers. The two are kept deliberately alike so a picture that differs
 * between the CPU and GPU renderers points at the renderer, not at the
 * decode.
 *
 * All three transform paths end the same way. The shader produces a
 * position in real surface pixels with w still the clip w, and turns that
 * into GL clip space as (screen * w) mapped to NDC. For every path the
 * executor handles, screen*w is linear in the clip-space position, so the
 * GPU's own near-plane clipping is exact -- no CPU clipping is needed.
 *
 * Surface rows are top-first in guest memory and in the framebuffer objects
 * alike (y_ndc = 2y/H - 1), so a surface later sampled as a texture needs no
 * flip; only presentation flips.
 */
#include "gl_vsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *p;
    size_t left;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->overflow = 1;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

static uint32_t f(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

static const char *const s_swz = "xyzw";

/* Source A (0), B (1) or C (2) as a GLSL vec4 expression. */
static int vsh_double(void);
static int g_noconst;   /* RECOMP_VSH_DOUBLE=2: the copy reads no constants */
static void src(Out *o, const uint32_t *t, int which, int consts_local)
{
    uint32_t mux, reg, neg, sw;
    char comp[5];

    switch (which) {
    case 0:  mux = f(t, 2, 26, 2); reg = f(t, 2, 28, 4);
             neg = f(t, 1, 8, 1);  sw = f(t, 1, 0, 8);  break;
    case 1:  mux = f(t, 2, 11, 2); reg = f(t, 2, 13, 4);
             neg = f(t, 2, 25, 1); sw = f(t, 2, 17, 8); break;
    default: mux = f(t, 3, 28, 2);
             reg = (f(t, 2, 0, 2) << 2) | f(t, 3, 30, 2);
             neg = f(t, 2, 10, 1); sw = f(t, 2, 2, 8);  break;
    }
    comp[0] = s_swz[(sw >> 6) & 3];
    comp[1] = s_swz[(sw >> 4) & 3];
    comp[2] = s_swz[(sw >> 2) & 3];
    comp[3] = s_swz[sw & 3];
    comp[4] = 0;

    emit(o, "%s", neg ? "-" : "");
    if (mux == 1) {
        if (reg < 12)
            emit(o, "R%u.%s", reg, comp);
        else if (reg == 12)
            emit(o, "oPos.%s", comp);
        else
            emit(o, "vec4(0.0).%s", comp);
    } else if (mux == 2) {
        if (vsh_double())
            emit(o, "(v%u + kp).%s", f(t, 1, 9, 4), comp);
        else
            emit(o, "v%u.%s", f(t, 1, 9, 4), comp);
    } else if (mux == 3 && g_noconst) {
        emit(o, "kp.%s", comp);
    } else if (mux == 3) {
        uint32_t ci = f(t, 1, 13, 8);
        const char *arr = consts_local ? "cl" : "c";
        if (f(t, 3, 1, 1) && !(getenv("RECOMP_VSH_STATIC") && *getenv("RECOMP_VSH_STATIC") == '1'))   /* experiment: no a0 */
            emit(o, "%s[clamp(%u + a0, 0, 191)].%s", arr, ci, comp);
        else
            emit(o, "%s[%u].%s", arr, ci < 192 ? ci : 191, comp);
    } else {
        emit(o, "vec4(0.0).%s", comp);
    }
}

/* ".xz" for mask bits 3=x .. 0=w; empty when nothing is written. */
static void mask_str(uint32_t mask, char out[6])
{
    int i, n = 0;

    out[n++] = '.';
    for (i = 0; i < 4; i++)
        if (mask & (8u >> i))
            out[n++] = s_swz[i];
    out[n] = 0;
}

/* dst.mask = val.mask; */
static void write_masked(Out *o, const char *dst, uint32_t mask, const char *val)
{
    char m[6];

    if (!mask)
        return;
    mask_str(mask, m);
    emit(o, "        %s%s = (%s)%s;\n", dst, m, val, m);
}

static const char *const s_out_names[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3",
};

/* RECOMP_VSH_DOUBLE=1 (cost experiment): every program runs twice, the
 * second on perturbed inputs so it cannot be merged; the image stays. */
static int vsh_double(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VSH_DOUBLE"); on = e ? atoi(e) : 0; }
    return on;
}

int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };
    uint32_t pc;
    int writes_consts = 0;

    {   /* RECOMP_VSH_NULL=1 (cost experiment, wrong image): an empty program */
        const char *e = getenv("RECOMP_VSH_NULL");
        if (e && *e == '2') {   /* empty, but every attribute fetched */
            emit(&o, "void nv2a_program(void)\n{\n    oPos = vec4(v0.xy, 0.5, 1.0) + 1e-30 * (v1 + v2 + v3 + v4 + v5"
                     " + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15);\n}\n");
            return o.overflow ? -1 : (int)(cap - o.left);
        }
        if (e && *e == '1') {
            emit(&o, "void nv2a_program(void)\n{\n    oPos = vec4(v0.xy, 0.5, 1.0);\n}\n");
            return o.overflow ? -1 : (int)(cap - o.left);
        }
    }
    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        if (f(t, 3, 12, 4) && !f(t, 3, 11, 1))
            writes_consts = 1;
        if (f(t, 3, 0, 1))
            break;
    }

    if (vsh_double())
        emit(&o, "vec4 kp = vec4(0.0);\n");
again:
    emit(&o, "void nv2a_program%s(void)\n{\n", g_noconst ? "2" : "");
    emit(&o, "    int a0 = 0;\n");
    if (writes_consts)
        emit(&o, "    vec4 cl[192];\n    for (int i = 0; i < 192; i++) cl[i] = c[i];\n");

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        uint32_t mac = f(t, 1, 21, 4), ilu = f(t, 1, 25, 3);
        uint32_t omask = f(t, 3, 12, 4);
        uint32_t rd = f(t, 3, 20, 4);

        emit(&o, "    { /* %u */\n        vec4 A = ", pc);
        src(&o, t, 0, writes_consts);
        emit(&o, ";\n        vec4 B = ");
        src(&o, t, 1, writes_consts);
        emit(&o, ";\n        vec4 C = ");
        src(&o, t, 2, writes_consts);
        emit(&o, ";\n        vec4 m = vec4(0.0), l = vec4(0.0);\n");

        if (getenv("RECOMP_VSH_NOALU") && *getenv("RECOMP_VSH_NOALU") == '1') {
            /* experiment (wrong image): same operands and data flow, adds only */
            if (mac && mac != 13) emit(&o, "        m = A + B + C;\n");
            if (ilu) emit(&o, "        l = C;\n");
            mac = mac == 13 ? 13 : mac ? 1 : 0;   /* keep a0 and the writes */
            if (mac == 1) mac = 100;
        }
        switch (mac) {
        case 100: break;
        case 1:  emit(&o, "        m = A;\n"); break;
        case 2:  emit(&o, "        m = A * B;\n"); break;
        case 3:  emit(&o, "        m = A + C;\n"); break;
        case 4:  emit(&o, "        m = A * B + C;\n"); break;
        case 5:  emit(&o, "        m = vec4(dot(A.xyz, B.xyz));\n"); break;
        case 6:  emit(&o, "        m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
        case 7:  emit(&o, "        m = vec4(dot(A, B));\n"); break;
        case 8:  emit(&o, "        m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  emit(&o, "        m = min(A, B);\n"); break;
        case 10: emit(&o, "        m = max(A, B);\n"); break;
        case 11: emit(&o, "        m = vec4(lessThan(A, B));\n"); break;
        case 12: emit(&o, "        m = vec4(greaterThanEqual(A, B));\n"); break;
        default: break;
        }
        switch (mac == 100 || (getenv("RECOMP_VSH_NOALU") && *getenv("RECOMP_VSH_NOALU") == '1') ? 0 : ilu) {
        case 1: emit(&o, "        l = C;\n"); break;
        case 2: emit(&o, "        l = vec4(nv2a_rcp(C.x));\n"); break;
        case 3: emit(&o, "        l = vec4(nv2a_rcc(C.x));\n"); break;
        case 4: emit(&o, "        l = vec4(nv2a_rcp(sqrt(abs(C.x))));\n"); break;
        case 5: emit(&o, "        l = nv2a_exp(C.x);\n"); break;
        case 6: emit(&o, "        l = nv2a_log(C.x);\n"); break;
        case 7: emit(&o, "        l = nv2a_lit(C);\n"); break;
        default: break;
        }

        if (mac == 13) {
            emit(&o, "        a0 = int(floor(A.x + 0.001));\n");
        } else if (mac && rd < 13) {
            char dst[8];
            if (rd == 12) snprintf(dst, sizeof dst, "oPos");
            else          snprintf(dst, sizeof dst, "R%u", rd);
            write_masked(&o, dst, f(t, 3, 24, 4), "m");
        }
        if (ilu) {
            uint32_t ld = mac ? 1 : rd;              /* paired ILU -> R1 */
            if (ld < 13) {
                char dst[8];
                if (ld == 12) snprintf(dst, sizeof dst, "oPos");
                else          snprintf(dst, sizeof dst, "R%u", ld);
                write_masked(&o, dst, f(t, 3, 16, 4), "l");
            }
        }
        if (omask) {
            const char *val = f(t, 3, 2, 1) ? "l" : "m";
            uint32_t addr = f(t, 3, 3, 8);
            if (f(t, 3, 11, 1)) {
                if (addr < 13)
                    write_masked(&o, s_out_names[addr], omask, val);
            } else {
                char dst[48];
                if (f(t, 3, 1, 1))
                    snprintf(dst, sizeof dst, "cl[clamp(%u + a0, 0, 191)]", addr);
                else
                    snprintf(dst, sizeof dst, "cl[%u]", addr < 192 ? addr : 191);
                write_masked(&o, dst, omask, val);
            }
        }
        emit(&o, "    }\n");
        if (f(t, 3, 0, 1))                          /* FINAL */
            break;
    }
    emit(&o, "}\n");
    if (vsh_double() == 2 && !g_noconst) {
        g_noconst = 1;
        goto again;
    }
    if (vsh_double() == 1)
        emit(&o, "void nv2a_program2(void) { nv2a_program(); }\n");
    g_noconst = 0;
    return o.overflow ? -1 : (int)(cap - o.left);
}

/* The NV2A rasterizer takes screen positions as fixed point with a 4-bit
 * fraction, truncated (xemu's roundScreenCoords). Xbox D3D's viewport
 * offset carries a +0.53125 bias, so a clip-space full-screen pass starts at
 * 0.53125 -- 0.5 on the hardware, which covers row and column 0; unsnapped,
 * GL left them out and NFSU2's glow buffer kept a stale edge that its
 * composite added back as a light line at the top and left. */
#define NV2A_SNAP_GLSL \
    "vec2 nv2a_snap(vec2 sw, float w) {\n" \
    "    return w > 0.0 ? trunc(sw / w * 16.0) / 16.0 * w : sw;\n" \
    "}\n"

/* Everything around the program body: inputs, the register file, the ILU
 * helpers (same constants as the interpreter), and the three position
 * paths. u_xform selects the path at run time so one program object serves
 * a vertex program under any surface. */
/* Vulkan GLSL (nv2a_vk): the same shader with explicit locations, the
 * uniforms in a std140 block (binding 0) and Vulkan's 0..1 clip depth. */
static const char s_vk_prelude[] =
    "#version 450\n"
    "layout(location = 0) in vec4 v0;\n"
    "layout(location = 1) in vec4 v1;\n"
    "layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3;\n"
    "layout(location = 4) in vec4 v4;\n"
    "layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6;\n"
    "layout(location = 7) in vec4 v7;\n"
    "layout(location = 8) in vec4 v8;\n"
    "layout(location = 9) in vec4 v9;\n"
    "layout(location = 10) in vec4 v10;\n"
    "layout(location = 11) in vec4 v11;\n"
    "layout(location = 12) in vec4 v12;\n"
    "layout(location = 13) in vec4 v13;\n"
    "layout(location = 14) in vec4 v14;\n"
    "layout(location = 15) in vec4 v15;\n"
    "layout(std140, set = 0, binding = 0) uniform VsU {\n"
    "    vec4 c[192];\n"
    "    vec4 u_surf;\n"
    "    vec4 u_m[4];\n"
    "    vec4 u_vpoff;\n"
    "    vec2 u_aa;\n"
    "    int u_xform;\n"
    "};\n"
    "layout(location = 0) out vec4 vD0; layout(location = 1) out vec4 vD1;\n"
    "layout(location = 2) out vec4 vT0; layout(location = 3) out vec4 vT1;\n"
    "layout(location = 4) out vec4 vT2; layout(location = 5) out vec4 vT3;\n"
    "layout(location = 6) out float vFog;\n"
    /* Multi-pass draws (depth EQUAL) recompute the same position in another
     * program: without invariance NVK compiles them differently and the later
     * passes drop pixels along edges (dark dashes on the console). */
    "invariant gl_Position;\n"
    "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
    "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
    "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "float nv2a_rcc(float x) {\n"
    "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "    return y < 0.0 ? -a : a;\n"
    "}\n"
    "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 nv2a_log(float x) {\n"
    "    float a = abs(x);\n"
    "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
    "    float e = floor(log2(a));\n"
    "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 nv2a_lit(vec4 s) {\n"
    "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
    "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
    "}\n"
    /* z clamped only in front of the eye (w > 0). Behind it (w < 0) the
     * vertex is clipped by x/y anyway and its true z is about w: clamping it
     * to 0 skewed the interpolated depth of triangles crossing the eye plane
     * (walls in hood view turned transparent, objects showed through them). */
    NV2A_SNAP_GLSL
    "vec4 nv2a_clip(vec3 sw, float w) {\n"
    "    float z = sw.z * u_surf.z;\n"
    "    sw.xy = nv2a_snap(sw.xy, w);\n"
    "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
    "                w > 0.0 ? clamp(z, 0.0, w) : z, w);\n"
    "}\n";

int nv2a_shader_vk;

const char *nv2a_gl_vsh_prelude(void)
{
    if (nv2a_shader_vk)
        return s_vk_prelude;
    return
        "#version 330 core\n"
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "layout(location = 2) in vec4 v2;\n"
        "layout(location = 3) in vec4 v3;\n"
        "layout(location = 4) in vec4 v4;\n"
        "layout(location = 5) in vec4 v5;\n"
        "layout(location = 6) in vec4 v6;\n"
        "layout(location = 7) in vec4 v7;\n"
        "layout(location = 8) in vec4 v8;\n"
        "layout(location = 9) in vec4 v9;\n"
        "layout(location = 10) in vec4 v10;\n"
        "layout(location = 11) in vec4 v11;\n"
        "layout(location = 12) in vec4 v12;\n"
        "layout(location = 13) in vec4 v13;\n"
        "layout(location = 14) in vec4 v14;\n"
        "layout(location = 15) in vec4 v15;\n"
        "uniform vec4 c[192];\n"
        "uniform vec4 u_surf;      /* 2/W, 2/H (real pixels), 1/zmax, 0 */\n"
        "uniform vec2 u_aa;        /* logical -> real pixels */\n"
        "uniform vec4 u_m[4];      /* fixed-function composite rows */\n"
        "uniform vec4 u_vpoff;     /* fixed-function viewport offset */\n"
        "out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1;\n"
        "out vec4 vT2; out vec4 vT3; out float vFog;\n"
        "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
        "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
        "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
        "float nv2a_rcc(float x) {\n"
        "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
        "    return y < 0.0 ? -a : a;\n"
        "}\n"
        "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
        "vec4 nv2a_log(float x) {\n"
        "    float a = abs(x);\n"
        "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
        "    float e = floor(log2(a));\n"
        "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
        "}\n"
        "vec4 nv2a_lit(vec4 s) {\n"
        "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
        "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
        "}\n"
        "/* screen*w (real pixels) and clip w to GL clip space. */\n"
        /* z clamped to the clip volume: the NV2A clamps depth rather than
         * clipping at the near and far planes, and GL_DEPTH_CLAMP alone did
         * not reach every driver -- on Eden NFSU2's loading screen, drawn at
         * z = 1.0, still vanished. */
        NV2A_SNAP_GLSL
        "vec4 nv2a_clip(vec3 sw, float w) {\n"
        "    float z = sw.z * u_surf.z * 2.0 - w;\n"
        "    sw.xy = nv2a_snap(sw.xy, w);\n"
        "    sw.xy += vec2(u_surf.w) * w;\n"
        "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
        "                clamp(z, -abs(w), abs(w)), w);\n"
        "}\n";
}

/* Render scale applied to oPts; set by nv2a_gl at init. */
float nv2a_point_scale = 1.0f;

const char *nv2a_gl_vsh_main_program(void)
{
    static char main_buf[2400];
    /* RECOMP_GL_ABSW=1 (diagnostic): rebuild clip space with |w|. The VP tail
     * has already divided by w (oPos.xy is screen space), so on the NV2A
     * the sign of w does not decide visibility; GL clips every w < 0
     * vertex. Conker's title scene has w < 0 throughout. */
    const char *e = getenv("RECOMP_GL_ABSW");
    if (e && *e == '1')
        return
            "void main() {\n"
            "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = R10 = R11 = vec4(0.0);\n"
            "    oPos = o1 = o2 = oFog = oPts = vec4(0.0);\n"
            "    oD0 = oD1 = oB0 = oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "    oT0 = oT1 = oT2 = oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "    nv2a_program();\n"
            "    float w = abs(oPos.w) < 1e-6 ? 1e-6 : abs(oPos.w);\n"
            "    vec3 s = vec3(oPos.xy * u_aa, oPos.z);\n"
            "    gl_Position = nv2a_clip(s * w, w);\n"
            "    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n"
            "    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3; vFog = oFog.x;\n"
            "}\n";
    /* gl_PointSize from oPts.x: a program drawing points sizes them there
     * (Conker's slingshot projectiles; GL drew them one pixel wide). The
     * size is in the title's pixels; surfaces are at the render scale. */
    snprintf(main_buf, sizeof main_buf,
        "void main() {\n"
        "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = R10 = R11 = vec4(0.0);\n"
        "    oPos = o1 = o2 = oFog = oPts = vec4(0.0);\n"
        "    oD0 = oD1 = oB0 = oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    oT0 = oT1 = oT2 = oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "%s"
        "    float w = abs(oPos.w) < 1e-6 ? 1e-6 : oPos.w;\n"
        "    vec3 s = vec3(oPos.xy * u_aa, oPos.z);\n"
        "    gl_Position = nv2a_clip(s * w, w);\n"
        "    gl_PointSize = clamp(oPts.x, 1.0, 64.0) * %.4f;\n"
        "    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n"
        "    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3; vFog = oFog.x;\n"
        "}\n", vsh_double() ?
        "    nv2a_program();\n"
        "    vec4 s0 = oPos, s1 = oD0, s2 = oD1, s3 = oT0, s4 = oT1, s5 = oT2, s6 = oT3, s7 = oFog, s8 = oPts;\n"
        "    kp = vec4(oPos.x * 1e-30);\n"
        "    nv2a_program2();\n"
        "    oPos = s0 + oPos * 1e-30; oD0 = s1 + oD0 * 1e-30; oD1 = s2 + oD1 * 1e-30;\n"
        "    oT0 = s3 + oT0 * 1e-30; oT1 = s4 + oT1 * 1e-30; oT2 = s5 + oT2 * 1e-30; oT3 = s6 + oT3 * 1e-30;\n"
        "    oFog = s7 + oFog * 1e-30; oPts = s8;\n"
        : "    nv2a_program();\n", nv2a_point_scale);
    return main_buf;
}

const char *nv2a_gl_vsh_fixed(void)
{
    static char result[16384];
    static const char fog[] =
        "uniform vec4 u_ffog[7];\n"
        "uniform vec4 u_flight[72];\n"
        "vec3 nv2a_material(int source) { return source == 1 ? v3.rgb : source == 2 ? v4.rgb : vec3(1.0); }\n"
        "vec3 nv2a_specular_rgb() {\n"
        "    vec3 color = vec3(0.0);\n"
        "    if (u_flight[66].y == 0.0) return color;\n"
        "    vec3 eye = vec3(dot(u_ffog[0],v0),dot(u_ffog[1],v0),dot(u_ffog[2],v0));\n"
        "    vec3 normal = vec3(dot(u_flight[67].xyz,v2.xyz),dot(u_flight[68].xyz,v2.xyz),dot(u_flight[69].xyz,v2.xyz));\n"
        "    if (u_flight[66].z != 0.0) normal = normalize(normal);\n"
        "    int flags = int(u_flight[70].w), mask = int(u_flight[66].x);\n"
        "    vec3 material = nv2a_material((int(u_flight[66].w)>>6)&3);\n"
        "    for (int i=0;i<8;i++) {\n"
        "        int type = (mask>>(2*i))&3, b=i*8;\n"
        "        vec3 ls = vec3(u_flight[b+1].zw,u_flight[b+2].x);\n"
        "        if (type == 0 || all(equal(ls,vec3(0.0)))) continue;\n"
        "        vec3 direction, halfvector; float attenuation = 1.0;\n"
        "        if (type == 1) direction = normalize(u_flight[b+3].yzw);\n"
        "        else {\n"
        "            vec3 delta = vec3(u_flight[b+5].w,u_flight[b+6].xy)-eye; float d=length(delta);\n"
        "            if (d > u_flight[b+2].y) continue;\n"
        "            direction = d > 0.0 ? delta/d : vec3(0.0);\n"
        "            attenuation = 1.0/dot(vec3(u_flight[b+6].zw,u_flight[b+7].x),vec3(1.0,d,d*d));\n"
        "            if (type == 3) {\n"
        "                vec4 spot=vec4(u_flight[b+4].w,u_flight[b+5].xyz);\n"
        "                attenuation *= clamp(dot(spot.xyz,direction)+spot.w,0.0,1.0);\n"
        "            }\n"
        "        }\n"
        "        if ((flags&4)!=0) halfvector=normalize(direction+normalize(u_flight[71].xyz/u_flight[71].w-eye));\n"
        "        else if (type==1) halfvector=vec3(u_flight[b+2].zw,u_flight[b+3].x);\n"
        "        else halfvector=direction;\n"
        "        float nh=max(dot(normal,halfvector),0.0), pf=0.0;\n"
        "        if (dot(normal,direction)>0.0 && nh > -u_flight[70].x) {\n"
        "            float denominator=u_flight[70].y*nh+u_flight[70].z;\n"
        "            if (denominator==0.0) pf=10000.0;\n"
        "            else if (denominator>0.0) pf=clamp((nh+u_flight[70].x)/denominator,0.0,10000.0);\n"
        "        }\n"
        "        color += material*ls*attenuation*pf;\n"
        "    }\n"
        "    return color;\n"
        "}\n"
        "vec4 nv2a_fixed_specular() {\n"
        "    int flags=int(u_flight[70].w);\n"
        "    if ((flags&1)==0) return vec4(0.0,0.0,0.0,1.0);\n"
        "    if (u_flight[66].y==0.0 || (flags&2)==0) return clamp(v4,0.0,1.0);\n"
        "    return clamp(vec4(nv2a_specular_rgb(),v4.a),0.0,1.0);\n"
        "}\n"
        "vec4 nv2a_fixed_diffuse() {\n"
        "    if (u_flight[66].y == 0.0) return clamp(v3,0.0,1.0);\n"
        "    vec3 eye = vec3(dot(u_ffog[0],v0),dot(u_ffog[1],v0),dot(u_ffog[2],v0));\n"
        "    vec3 normal = vec3(dot(u_flight[67].xyz,v2.xyz),dot(u_flight[68].xyz,v2.xyz),dot(u_flight[69].xyz,v2.xyz));\n"
        "    if (u_flight[66].z != 0.0) normal = normalize(normal);\n"
        "    int material = int(u_flight[66].w), mask = int(u_flight[66].x);\n"
        "    vec3 diffuse = nv2a_material((material >> 4) & 3);\n"
        "    vec3 ambient = nv2a_material((material >> 2) & 3);\n"
        "    vec3 color = nv2a_material(material & 3)*u_flight[65].rgb + ambient*u_flight[64].rgb;\n"
        "    for (int i=0;i<8;i++) {\n"
        "        int type = (mask >> (2*i)) & 3, b = i*8;\n"
        "        if (type == 0) continue;\n"
        "        vec3 direction; float attenuation = 1.0;\n"
        "        if (type == 1) direction = vec3(u_flight[b+3].yzw);\n"
        "        else {\n"
        "            vec3 delta = vec3(u_flight[b+5].w,u_flight[b+6].xy)-eye; float d = length(delta);\n"
        "            if (d > u_flight[b+2].y) continue;\n"
        "            direction = d > 0.0 ? delta/d : vec3(0.0);\n"
        "            attenuation = 1.0/dot(vec3(u_flight[b+6].zw,u_flight[b+7].x),vec3(1.0,d,d*d));\n"
        "            if (type == 3) {\n"
        "                vec4 spot = vec4(u_flight[b+4].w,u_flight[b+5].xyz);\n"
        "                attenuation *= clamp(dot(spot.xyz,direction)+spot.w,0.0,1.0);\n"
        "            }\n"
        "        }\n"
        "        vec3 la = u_flight[b].xyz, ld = vec3(u_flight[b].w,u_flight[b+1].xy);\n"
        "        color += attenuation*(ambient*la + diffuse*ld*max(dot(normal,direction),0.0));\n"
        "    }\n"
        "    if ((int(u_flight[70].w)&3)==1) color += nv2a_specular_rgb();\n"
        "    return clamp(vec4(color,((material>>4)&3)==1?v3.a:((material>>4)&3)==2?v4.a:u_flight[65].a),0.0,1.0);\n"
        "}\n"
        "float nv2a_fixed_fog() {\n"
        "    if (u_ffog[5].w == 0.0) return 1.0;\n"
        "    vec3 eye = vec3(dot(u_ffog[0],v0),dot(u_ffog[1],v0),dot(u_ffog[2],v0));\n"
        "    int gen = int(u_ffog[6].x), mode = int(u_ffog[5].z);\n"
        "    float d = v4.a;\n"
        "    if (gen == 1) d = length(eye);\n"
        "    else if (gen == 2 || gen == 3) {\n"
        "        d = dot(u_ffog[4].xyz,eye) + u_ffog[4].w;\n"
        "        if (gen == 3) d = abs(d);\n"
        "    } else if (gen == 6) d = v5.x;\n"
        "    float f;\n"
        "    if (mode == 9729 || mode == 2052) f = u_ffog[5].x + d*u_ffog[5].y - 1.0;\n"
        "    else if (mode == 2048 || mode == 2050) f = u_ffog[5].x + exp2(d*u_ffog[5].y*16.0) - 1.5;\n"
        "    else f = u_ffog[5].x + exp2(-d*d*u_ffog[5].y*u_ffog[5].y*32.0) - 1.5;\n"
        "    if (mode == 2050 || mode == 2051 || mode == 2052) f = abs(f);\n"
        "    if (isinf(d) || isnan(f)) return (mode == 9729 || mode == 2052 || mode == 2048) ? 1.0 : 0.0;\n"
        "    return clamp(f,-3.402823466e38,3.402823466e38);\n"
        "}\n";
    /* Vulkan: u_xform is a member of the prelude's uniform block. */
    static const char decl[] = "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n";
    static const char body[] =
        "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n"
        "void main() {\n"
        "    if (u_xform == 1) {\n"
        "        vec4 clip = vec4(dot(u_m[0], v0), dot(u_m[1], v0),\n"
        "                         dot(u_m[2], v0), dot(u_m[3], v0));\n"
        "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
        "        vec3 sw = clip.xyz + u_vpoff.xyz * w;\n"
        "        sw.xy *= u_aa;\n"
        "        gl_Position = nv2a_clip(sw, w);\n"
        "    } else {\n"
        "        float w = (v0.w > 0.0) ? 1.0 / v0.w : 1.0;\n"
        "        gl_Position = nv2a_clip(v0.xyz * w, w);\n"
        "    }\n"
        "    vD0 = nv2a_fixed_diffuse(); vD1 = nv2a_fixed_specular();\n"
        "    vT0 = v9; vT1 = v10; vT2 = v11; vT3 = v12; vFog = nv2a_fixed_fog();\n"
        "}\n";
    /* Vulkan's uniform block has not acquired fixed-function fog state yet. */
    snprintf(result, sizeof result, "%s%s", nv2a_shader_vk
             ? "float nv2a_fixed_fog() { return 0.0; }\nvec4 nv2a_fixed_diffuse() { return clamp(v3,0.0,1.0); }\nvec4 nv2a_fixed_specular() { return clamp(v4,0.0,1.0); }\n" : fog,
             nv2a_shader_vk ? body + sizeof decl - 1 : body);
    return result;
}
