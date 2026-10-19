/**
 * nv2a_fixture.c -- capture and replay of what a render back end is given.
 *
 * A fixture is the sequence of back-end calls (clear, draw_raw, flip) for a
 * few frames, with the guest memory those calls read (texture images,
 * palettes, vertex arrays stored as the title left them). Replaying it drives
 * the same back end without the title: the same frames every time, on any
 * host the binary runs on (a capture from the console replays on Linux and
 * back; both are 64-bit little-endian with the same struct layout). Used to
 * compare renderers and drivers pixel by pixel and to time the renderer
 * alone (modelled on the UFC3 port's fixtures).
 *
 *   RECOMP_CAPTURE=<file>  RECOMP_CAPTURE_AT=<first frame>[,<frames>]  record (0, 3)
 *     RECOMP_CAPTURE_AT=pad[,<frames>]: start when L + R + left-stick click are held
 *     (Switch: xinput_nx.c sets nv2a_fixture_pad_trigger), once
 *   RECOMP_REPLAY=<file>[|<file>...]  RECOMP_REPLAY_LOOPS=<n>  replay instead of booting
 *     (several fixtures play one after another, each <n> times; frames are
 *     numbered across all of them; timing is summarised per fixture and loop)
 * (separate variables: paths may contain commas)
 *   RECOMP_REPLAY_OUT=<file>                           timing summary there too
 *
 * Frames are counted at flip. Replay times each frame (wall clock from the
 * first back-end call after a flip to the end of the next flip, without the
 * fixture's own memory writes); dump frames with the back end's own switch
 * (RECOMP_GL_DUMP=<prefix>,1).
 *
 * The file is gzip (level 1, ~6x smaller: the console copies it over FTP);
 * replay also reads it uncompressed.
 *
 * Memory: before each call, every range it reads is hashed; a range whose
 * contents differ from what the fixture last stored for it is stored again.
 * Replay writes those ranges back into guest memory before the call.
 */
#include "nv2a_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static void renderer_drain(void)
{
    extern void nv2a_gl_queue_drain(void) __attribute__((weak));
    nv2a_queue_drain();
    if (nv2a_gl_queue_drain) nv2a_gl_queue_drain();
}

#define FIX_MAGIC   0x3258494Bu         /* "KIX2": inline vertex arrays */
enum { REC_MEM = 1, REC_DRAW = 2, REC_CLEAR = 3, REC_FLIP = 4 };

static const Nv2aBackend *s_real;
static gzFile s_cap;
static uint32_t s_cap_first, s_cap_frames, s_cap_flips;
static int s_cap_on;                    /* inside the captured frames */
static int s_cap_pad;                   /* armed: wait for the pad trigger */
volatile int nv2a_fixture_pad_trigger;
uint64_t nv2a_fixture_mem_ns;           /* replay: guest-memory write-back so far */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Capture ──────────────────────────────────────────────────────── */

static void rec(uint32_t type, const void *a, uint32_t na, const void *b, uint32_t nb)
{
    uint32_t h[2] = { type, na + nb };
    gzwrite(s_cap, h, (unsigned)((sizeof h) * (1)));
    if (na) gzwrite(s_cap, a, (unsigned)((1) * (na)));
    if (nb) gzwrite(s_cap, b, (unsigned)((1) * (nb)));
}

/* Ranges already in the fixture: (va, len) -> content hash. */
#define MEM_SLOTS 8192
static struct { uint32_t va, len; uint64_t hash; } s_mem[MEM_SLOTS];

static uint64_t fnv64(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* Guest RAM and its contiguous mirror. The executor also hands back end
 * vertex arrays in buffers of its own (host memory): those go into the draw
 * record itself -- recorded as guest offsets they pointed nowhere on the
 * console (the replay faulted writing them back). */
static int guest_range(uint64_t va, uint32_t len)
{
    uint64_t end = va + len;
    return (end <= 0x08000000ull) || (va >= 0x80000000ull && end <= 0x88000000ull);
}

static void cap_range(uint32_t va, uint32_t len)
{
    const uint8_t *p;
    uint64_t h;
    uint32_t slot, probe;
    if (!va || !len || len > (32u << 20) || !guest_range(va, len))
        return;
    p = (const uint8_t *)xbox_GetMemoryOffset() + va;
    h = fnv64(p, len);
    slot = (va * 2654435761u ^ len) & (MEM_SLOTS - 1);
    for (probe = 0; probe < 16; probe++, slot = (slot + 1) & (MEM_SLOTS - 1)) {
        if (s_mem[slot].len == 0 || (s_mem[slot].va == va && s_mem[slot].len == len))
            break;
    }
    if (probe < 16 && s_mem[slot].va == va && s_mem[slot].len == len && s_mem[slot].hash == h)
        return;                                 /* replay memory holds it already */
    if (probe < 16) {
        s_mem[slot].va = va; s_mem[slot].len = len; s_mem[slot].hash = h;
    }
    {
        uint32_t hdr[2] = { va, len };
        rec(REC_MEM, hdr, sizeof hdr, p, len);
    }
}

/* Bytes one stored attribute element takes (NV097 type codes, see
 * Nv2aRawBatch.direct). */
static uint32_t attr_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;                           /* D3DCOLOR */
    case 1: case 5: return 2 * size;            /* S1, S32K */
    case 2: return 4 * size;                    /* float */
    case 4: return size;                        /* unsigned byte */
    case 6: return 4;                           /* CMP (11:11:10) */
    default: return 4 * size;
    }
}

/* What a stage's texture occupies in guest memory, from the method shadow. */
static uint32_t tex_range_bytes(const uint32_t *regs, int i)
{
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t format = regs[base + 1], color = (format >> 8) & 0xFF;
    uint32_t bits = nv2a_texel_bits(color);
    if (bits) {                                 /* swizzled / compressed */
        uint32_t w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
        uint32_t levels = (format >> 16) & 0xF;
        uint32_t face = nv2a_cube_face_bytes(color, w, h, levels ? levels : 1);
        return (format & 4u) ? face * 6 : face;
    } else {                                    /* linear: pitch x height */
        uint32_t rect = regs[base + 7], pitch = regs[base + 4] >> 16;
        return pitch * (rect & 0xFFFF);
    }
}

static void cap_batch_memory(const Nv2aRawBatch *b)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
        if (!b->tex_va[i] || !(b->regs[base + 3] & 0x40000000u))
            continue;
        cap_range(b->tex_va[i], tex_range_bytes(b->regs, i));
        if (b->pal_va[i])
            cap_range(b->pal_va[i], 1024);
    }
    for (i = 0; i < NV2A_RAW_ATTRS; i++) {
        uint32_t len;
        if (!(b->attr_direct & (1u << i)) || !b->direct[i].ptr || !b->vertex_count)
            continue;
        len = (b->vertex_count - 1) * b->direct[i].stride + attr_bytes(b->direct[i].type, b->direct[i].size);
        if (guest_range((uint64_t)(b->direct[i].ptr - mem), len))
            cap_range((uint32_t)(b->direct[i].ptr - mem), len);
    }
}

/* Draw record: the struct (host pointers meaningless in the file), then
 * attrs, indices, regs, program, constants; direct[].ptr as guest VAs. */
typedef struct {
    Nv2aRawBatch b;
    uint32_t     attr_floats;           /* floats in attrs */
    uint32_t     direct_va[NV2A_RAW_ATTRS];
    uint32_t     inline_len[NV2A_RAW_ATTRS];  /* nonzero: array stored in the record */
} DrawHdr;

static uint32_t attr_float_count(const Nv2aRawBatch *b)
{
    uint32_t per = b->attrs_packed
                 ? (uint32_t)__builtin_popcount(b->attr_present & (uint16_t)~b->attr_direct)
                 : NV2A_RAW_ATTRS;
    return b->attrs ? b->vertex_count * per * 4 : 0;
}

static void cap_draw_raw(const Nv2aRawBatch *b)
{
    if (s_cap_on) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        DrawHdr h;
        uint32_t ia, ii, ir = 0x2000, ip, ic, i, h2[2], il = 0;
        cap_batch_memory(b);
        memset(&h, 0, sizeof h);
        h.b = *b;
        h.attr_floats = attr_float_count(b);
        for (i = 0; i < NV2A_RAW_ATTRS; i++) {
            uint32_t len;
            if (!(b->attr_direct & (1u << i)) || !b->direct[i].ptr || !b->vertex_count)
                continue;
            len = (b->vertex_count - 1) * b->direct[i].stride
                + attr_bytes(b->direct[i].type, b->direct[i].size);
            if (guest_range((uint64_t)(b->direct[i].ptr - mem), len))
                h.direct_va[i] = (uint32_t)(b->direct[i].ptr - mem);
            else {
                h.inline_len[i] = len;
                il += (len + 15) & ~15u;
            }
        }
        ia = h.attr_floats * 4;
        ii = b->indices ? b->index_count * 4 : 0;
        ip = b->vp_program ? b->vp_slots * 16 : 0;
        ic = b->vp_consts ? b->vp_const_count * 16 : 0;
        h2[0] = REC_DRAW;
        h2[1] = (uint32_t)sizeof h + ia + ii + ir + ip + ic + il;
        gzwrite(s_cap, h2, (unsigned)((sizeof h2) * (1)));
        gzwrite(s_cap, &h, (unsigned)((sizeof h) * (1)));
        if (ia) gzwrite(s_cap, b->attrs, (unsigned)((1) * (ia)));
        if (ii) gzwrite(s_cap, b->indices, (unsigned)((1) * (ii)));
        gzwrite(s_cap, b->regs, (unsigned)((1) * (ir)));
        if (ip) gzwrite(s_cap, b->vp_program, (unsigned)((1) * (ip)));
        if (ic) gzwrite(s_cap, b->vp_consts, (unsigned)((1) * (ic)));
        for (i = 0; i < NV2A_RAW_ATTRS; i++)
            if (h.inline_len[i]) {
                static const uint8_t pad[16];
                uint32_t len = h.inline_len[i];
                gzwrite(s_cap, b->direct[i].ptr, len);
                if (len & 15) gzwrite(s_cap, pad, 16 - (len & 15));
            }
    }
    s_real->draw_raw(b);
}

typedef struct {
    Nv2aSurface     s;
    Nv2aRenderState rs;
    uint32_t        flags, argb, zstencil, has_rs;
} ClearRec;

static void cap_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    if (s_cap_on) {
        ClearRec c;
        memset(&c, 0, sizeof c);
        if (s) c.s = *s;
        if (rs) { c.rs = *rs; c.has_rs = 1; }
        c.flags = flags; c.argb = argb; c.zstencil = zstencil;
        rec(REC_CLEAR, &c, sizeof c, NULL, 0);
    }
    s_real->clear(s, rs, flags, argb, zstencil);
}

static void cap_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    s_real->draw(s, b);                 /* CPU-transformed path: not captured */
}

static void cap_flip(void)
{
    if (s_cap_on)
        rec(REC_FLIP, NULL, 0, NULL, 0);
    s_real->flip();
    s_cap_flips++;
    if (s_cap_pad && !s_cap_on && s_cap && nv2a_fixture_pad_trigger) {
        s_cap_pad = 0;
        s_cap_first = s_cap_flips;              /* from the next frame */
    }
    if (!s_cap_on && !s_cap_pad && s_cap && s_cap_flips == s_cap_first) {
        s_cap_on = 1;
        fprintf(stderr, "[fixture] capturing %u frames from frame %u\n", s_cap_frames, s_cap_first);
    } else if (s_cap_on && s_cap_flips >= s_cap_first + s_cap_frames) {
        s_cap_on = 0;
        gzclose(s_cap);
        s_cap = NULL;
        fprintf(stderr, "[fixture] capture complete\n");
        if (getenv("RECOMP_EXIT_AFTER_CAPTURE"))    /* tools/capture_linux.sh */
            _Exit(0);
    }
}

static Nv2aBackend s_shim;

const Nv2aBackend *nv2a_fixture_wrap(const Nv2aBackend *real)
{
    const char *e = getenv("RECOMP_CAPTURE"), *at = getenv("RECOMP_CAPTURE_AT");
    char path[512];
    const char *c;
    if (!e || !*e || getenv("RECOMP_REPLAY") || !real->draw_raw)
        return real;
    snprintf(path, sizeof path, "%s", e);
    s_cap_pad = at && !strncmp(at, "pad", 3);
    s_cap_first = at && !s_cap_pad ? (uint32_t)strtoul(at, NULL, 10) : 0;
    c = at ? strchr(at, ',') : NULL;
    s_cap_frames = c ? (uint32_t)strtoul(c + 1, NULL, 10) : 3;
    if (!s_cap_frames) s_cap_frames = 3;
    if (!(s_cap = gzopen(path, "wb1"))) {
        fprintf(stderr, "[fixture] cannot write %s\n", path);
        return real;
    }
    {
        uint32_t hdr[4] = { FIX_MAGIC, (uint32_t)sizeof(Nv2aRawBatch), (uint32_t)sizeof(ClearRec), 0 };
        gzwrite(s_cap, hdr, (unsigned)((sizeof hdr) * (1)));
    }
    s_real = real;
    s_shim = *real;
    s_shim.clear = cap_clear;
    s_shim.draw = cap_draw;
    s_shim.flip = cap_flip;
    s_shim.draw_raw = cap_draw_raw;
    s_cap_on = !s_cap_pad && s_cap_first == 0;
    fprintf(stderr, "[fixture] capture to %s: frames %u..%u\n", path, s_cap_first,
            s_cap_first + s_cap_frames - 1);
    return &s_shim;
}

/* ── Replay ───────────────────────────────────────────────────────── */

/* The fixture, decompressed into memory before the first frame: reading and
 * inflating it from the SD card inside the timed frames was most of a
 * console replay's time. */
static uint8_t *s_fix;
static size_t   s_fix_len;

static int load_fixture(const char *path)
{
    gzFile f = gzopen(path, "rb");
    size_t cap = 64u << 20;
    int n;
    if (!f)
        return 0;
    free(s_fix);
    s_fix_len = 0;
    s_fix = (uint8_t *)malloc(cap);
    while (s_fix && (n = gzread(f, s_fix + s_fix_len, (unsigned)(cap - s_fix_len))) > 0) {
        s_fix_len += (size_t)n;
        if (s_fix_len == cap) {
            uint8_t *g = (uint8_t *)realloc(s_fix, cap * 2);
            if (!g) { free(s_fix); s_fix = NULL; break; }
            s_fix = g;
            cap *= 2;
        }
    }
    gzclose(f);
    return s_fix != NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int replay_one(const Nv2aBackend *be, const char *e, FILE *out);

/* Returns the process exit code. */
int nv2a_fixture_replay_main(const Nv2aBackend *be)
{
    const char *list = getenv("RECOMP_REPLAY"), *outp = getenv("RECOMP_REPLAY_OUT");
    char one[512];
    FILE *out = outp && *outp ? fopen(outp, "w") : NULL;
    int rc = 0;
    while (list && *list) {
        const char *bar = strchr(list, '|');
        size_t n = bar ? (size_t)(bar - list) : strlen(list);
        if (n >= sizeof one) n = sizeof one - 1;
        memcpy(one, list, n);
        one[n] = 0;
        if (n) {
            /* A clean renderer per fixture: cached textures and render targets
             * of the previous one live at the same guest addresses. */
            extern void nv2a_vk_reset_resources(void) __attribute__((weak));
            renderer_drain();
            if (nv2a_vk_reset_resources)
                nv2a_vk_reset_resources();
            rc |= replay_one(be, one, out);
        }
        list = bar ? bar + 1 : NULL;
    }
    if (out) fclose(out);
    return rc;
}

static int replay_one(const Nv2aBackend *be, const char *e, FILE *out)
{
    char path[512];
    static uint32_t frame;              /* across fixtures */
    uint32_t hdr[4], loops, loop, nframes = 0, draws = 0, clears = 0;
    uint64_t *ft = NULL;
    size_t ft_cap = 0;
    uint64_t t_frame = 0, t_mem = 0, last[256];
    uint32_t last_n = 0;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();

    snprintf(path, sizeof path, "%s", e);
    loops = getenv("RECOMP_REPLAY_LOOPS") ? (uint32_t)strtoul(getenv("RECOMP_REPLAY_LOOPS"), NULL, 10) : 1;
    if (!loops) loops = 1;
    uint64_t tw0 = 0, tw1;
    uint32_t fw0 = 0;
    {
        uint64_t tl = now_ns();
        if (!load_fixture(path) || s_fix_len < sizeof hdr) {
            fprintf(stderr, "[replay] %s: cannot read\n", path);
            return 2;
        }
        memcpy(hdr, s_fix, sizeof hdr);
        if (hdr[0] != FIX_MAGIC) {
            fprintf(stderr, "[replay] %s: not a fixture\n", path);
            return 2;
        }
        fprintf(stderr, "[replay] %s: %zu bytes loaded in %.0f ms\n", path, s_fix_len, (now_ns() - tl) / 1e6);
    }
    if (hdr[1] != sizeof(Nv2aRawBatch) || hdr[2] != sizeof(ClearRec)) {
        fprintf(stderr, "[replay] %s: recorded by another build (batch %u/%u bytes)\n",
                path, hdr[1], (unsigned)sizeof(Nv2aRawBatch));
        return 2;
    }
    fprintf(stderr, "[replay] %s, %u loop(s)\n", path, loops);
    for (loop = 0; loop < loops; loop++) {
        uint32_t rh[2];
        if (loop == loops / 2) {            /* throughput: the second half, drained */
            extern int nv2a_vk_gpu_stats(double *, double *) __attribute__((weak));
            extern int nv2a_vk_cpu_stats(char *, size_t) __attribute__((weak));
            char junk[1800];
            double ja, jm, q1, q2, q3;
            renderer_drain();
            /* the reported stats cover the warm half only */
            if (nv2a_vk_cpu_stats) nv2a_vk_cpu_stats(junk, sizeof junk);
            if (nv2a_vk_gpu_stats) nv2a_vk_gpu_stats(&ja, &jm);
            if (nv2a_queue_active()) nv2a_queue_stats(1, &q1, &q2, &q3);
            {
                extern void nv2a_vk_pass_report(FILE *, int) __attribute__((weak));
                if (nv2a_vk_pass_report) nv2a_vk_pass_report(NULL, 0);
            }
            tw0 = now_ns();
            fw0 = nframes;
        }
        uint64_t t0 = 0, tm;
        size_t at = sizeof hdr;
        while (at + sizeof rh <= s_fix_len) {
            uint8_t *s_buf;
            memcpy(rh, s_fix + at, sizeof rh);
            at += sizeof rh;
            if (at + rh[1] > s_fix_len)
                break;
            s_buf = s_fix + at;                 /* records are 4-byte aligned in the file */
            at += rh[1];
            if (rh[0] == REC_MEM) {
                uint32_t va, len;
                /* Recorded writes may replace a movie's single texture buffer.
                 * Queued draws still sample guest RAM, so finish consuming the
                 * previous contents before restoring the next snapshot. */
                renderer_drain();
                tm = now_ns();
                memcpy(&va, s_buf, 4);
                memcpy(&len, s_buf + 4, 4);
                memcpy(mem + va, s_buf + 8, len);
                tm = now_ns() - tm;
                t_mem += tm;
                nv2a_fixture_mem_ns += tm;      /* RECOMP_VK_FTRACE: replay-only time per frame */
                continue;
            }
            if (!t0)
                t0 = now_ns();
            if (rh[0] == REC_DRAW) {
                DrawHdr *h = (DrawHdr *)s_buf;
                Nv2aRawBatch b = h->b;
                uint8_t *p = s_buf + sizeof *h;
                uint32_t i;
                b.attrs = h->attr_floats ? (const float *)p : NULL;
                p += h->attr_floats * 4;
                if (b.indices) { b.indices = (const uint32_t *)p; p += b.index_count * 4; }
                b.regs = (const uint32_t *)p;
                p += 0x2000;
                if (b.vp_program) { b.vp_program = (const uint32_t (*)[4])p; p += b.vp_slots * 16; }
                if (b.vp_consts) { b.vp_consts = (const float (*)[4])p; p += b.vp_const_count * 16; }
                for (i = 0; i < NV2A_RAW_ATTRS; i++) {
                    if (h->inline_len[i]) {
                        b.direct[i].ptr = p;
                        p += (h->inline_len[i] + 15) & ~15u;
                    } else {
                        b.direct[i].ptr = h->direct_va[i] ? mem + h->direct_va[i] : NULL;
                    }
                }
                {   /* nv2a_raw_dirty: block diffs against the previous draw,
                     * computed in the first loop (warm-up), reused after */
                    static uint64_t *dm;
                    static uint32_t dm_cap;
                    static const uint32_t *prev;
                    static uint32_t last_loop = ~0u, d0;
                    uint32_t di;
                    if (loop != last_loop) { last_loop = loop; d0 = draws; }
                    di = draws - d0;
                    if (di >= dm_cap) {
                        uint32_t cap = dm_cap ? dm_cap * 2 : 4096;
                        uint64_t *d = (uint64_t *)realloc(dm, (size_t)cap * 16);
                        if (d) { memset(d + (size_t)dm_cap * 2, 0xFF, (size_t)(cap - dm_cap) * 16); dm = d; dm_cap = cap; }
                    }
                    if (dm && di < dm_cap) {
                        if (loop == 0) {
                            uint64_t *m = dm + (size_t)di * 2;
                            uint32_t c;
                            m[0] = m[1] = 0;
                            for (c = 0; c < 128; c++)
                                if (!prev || memcmp(b.regs + c * 16, prev + c * 16, 64))
                                    m[c >> 6] |= 1ull << (c & 63);
                            prev = b.regs;
                        }
                        /* Not with the render thread on: it owns the global (its own
                         * mask per record), and writing it here raced its reads -- a
                         * NULL dereference in pd_join on the console. */
                        if (!nv2a_queue_active())
                            nv2a_raw_dirty = di ? dm + (size_t)di * 2 : NULL;
                    }
                    {   /* RECOMP_REPLAY_DRAWLOG=1: target and textures of each draw */
                        static int dl = -1;
                        if (dl < 0) dl = getenv("RECOMP_REPLAY_DRAWLOG") != NULL;
                        if (dl)
                            fprintf(stderr, "[draw] f%u d%u col %08X zeta %08X %ux%u tex %08X %08X %08X %08X fmt0 %08X v%u\n",
                                    nframes, draws, b.surface.color_va, b.zeta_va, b.surface.width,
                                    b.surface.height, b.tex_va[0], b.tex_va[1], b.tex_va[2], b.tex_va[3],
                                    b.regs[(0x1B04) / 4], b.vertex_count);
                    }
                    {   /* RECOMP_REPLAY_REGDUMP=<draw>: that draw's nonzero method registers */
                        const char *e = getenv("RECOMP_REPLAY_REGDUMP");
                        if (e && (uint32_t)strtoul(e, NULL, 10) == draws) {
                            uint32_t k;
                            fprintf(stderr, "[regs] draw %u:", draws);
                            for (k = 0; k < 0x2000 / 4; k++)
                                if (b.regs[k])
                                    fprintf(stderr, " %04X=%08X", k * 4, b.regs[k]);
                            fprintf(stderr, "\n");
                        }
                    }
                    be->draw_raw(&b);
                    if (!nv2a_queue_active())
                        nv2a_raw_dirty = NULL;
                    {   /* RECOMP_REPLAY_DUMPVA=<hex va>[,<dir>]: that render target
                         * after every draw into it (diagnostic, VK) */
                        extern int nv2a_vk_dump_va(uint32_t, const char *) __attribute__((weak));
                        static int dv = -1;
                        static uint32_t dva, devery = 1, dmaxf = ~0u, dminf = 0;
                        static char ddir[200];
                        if (dv < 0) {
                            const char *e = getenv("RECOMP_REPLAY_DUMPVA"), *c;
                            dv = e && *e && nv2a_vk_dump_va;
                            if (dv) {
                                dva = (uint32_t)strtoul(e, NULL, 16);
                                c = strchr(e, ',');
                                snprintf(ddir, sizeof ddir, "%s", c ? c + 1 : "/tmp");
                                if (strchr(ddir, ',')) {       /* ,<dir>,<every>,<frames> */
                                    char *q = strchr(ddir, ',');
                                    *q++ = 0;
                                    devery = (uint32_t)strtoul(q, &q, 10);
                                    if (!devery) devery = 1;
                                    if (*q == ',') dmaxf = (uint32_t)strtoul(q + 1, &q, 10);
                                    if (*q == ',') dminf = (uint32_t)strtoul(q + 1, NULL, 10);
                                }
                            }
                        }
                        if (dv && b.surface.color_va == dva && draws % devery == 0 && nframes < dmaxf && nframes >= dminf) {
                            char path[300];
                            snprintf(path, sizeof path, "%s/d%u_%05u_%08X.bmp", ddir, nframes, draws, dva);
                            nv2a_vk_dump_va(dva, path);
                        }
                    }
                }
                draws++;
            } else if (rh[0] == REC_CLEAR) {
                ClearRec *cr = (ClearRec *)s_buf;
                if (getenv("RECOMP_REPLAY_DRAWLOG"))
                    fprintf(stderr, "[clear] f%u after d%u col %08X %ux%u flags %X argb %08X\n",
                            nframes, draws, cr->s.color_va, cr->s.width, cr->s.height,
                            cr->flags, cr->argb);
                be->clear(&cr->s, cr->has_rs ? &cr->rs : NULL, cr->flags, cr->argb, cr->zstencil);
                clears++;
            } else if (rh[0] == REC_FLIP) {
                uint64_t dt;
                be->flip();
                dt = now_ns() - t0;
                t0 = 0;
                if (nframes == ft_cap) {
                    ft_cap = ft_cap ? ft_cap * 2 : 256;
                    ft = (uint64_t *)realloc(ft, ft_cap * sizeof *ft);
                }
                if (ft) ft[nframes] = dt;
                if (loop == loops - 1 && last_n < 256)
                    last[last_n++] = dt;
                nframes++;
                t_frame += dt;
                fprintf(stderr, "[replay] loop %u frame %u: %.2f ms\n", loop, frame, dt / 1e6);
                frame++;
            }
        }
    }
    renderer_drain();
    tw1 = now_ns();
    if (nframes && ft) {
        double med, p95, avg = t_frame / 1e6 / nframes;
        char line[2048];
        if (nframes > fw0) {
            snprintf(line, sizeof line, "[replay] %s: throughput %.2f ms per frame (wall clock over the "
                     "last %u frames, queue drained)\n", path, (tw1 - tw0) / 1e6 / (nframes - fw0), nframes - fw0);
            fputs(line, stderr);
            if (out) fputs(line, out);
        }
        qsort(ft, nframes, sizeof *ft, cmp_u64);
        med = ft[nframes / 2] / 1e6;
        p95 = ft[(nframes * 95) / 100 < nframes ? (nframes * 95) / 100 : nframes - 1] / 1e6;
        snprintf(line, sizeof line,
                 "[replay] %s: %u frames (%u draws, %u clears): avg %.2f ms, median %.2f ms, p95 %.2f ms, "
                 "min %.2f, max %.2f; memory writes %.1f ms total\n",
                 path, nframes, draws, clears, avg, med, p95, ft[0] / 1e6, ft[nframes - 1] / 1e6, t_mem / 1e6);
        fputs(line, stderr);
        if (out) fputs(line, out);
        {
            extern int nv2a_vk_gpu_stats(double *, double *) __attribute__((weak));
            extern int nv2a_vk_cpu_stats(char *, size_t) __attribute__((weak));
            double ga, gm;
            char cs[1800];
            renderer_drain();
            if (nv2a_queue_active()) {
                double qi, qw, qc;
                nv2a_queue_stats(nframes - fw0, &qi, &qw, &qc);
                snprintf(line, sizeof line, "[replay] %s: queue per frame: render thread idle %.2f ms, "
                         "producer blocked %.2f ms, producer copying %.2f ms\n", path, qi, qw, qc);
                fputs(line, stderr);
                if (out) fputs(line, out);
            }
            if (nv2a_vk_cpu_stats && nv2a_vk_cpu_stats(cs, sizeof cs)) {
                snprintf(line, sizeof line, "[replay] %s\n", cs);
                fputs(line, stderr);
                if (out) fputs(line, out);
            }
            {
                extern void nv2a_vk_pass_report(FILE *, int) __attribute__((weak));
                if (nv2a_vk_pass_report) nv2a_vk_pass_report(out, 1);
            }
            if (nv2a_vk_gpu_stats && nv2a_vk_gpu_stats(&ga, &gm) > 0) {
                snprintf(line, sizeof line, "[replay] %s: GPU %.2f ms per frame average, worst %.2f ms\n",
                         path, ga, gm);
                fputs(line, stderr);
                if (out) fputs(line, out);
            }
        }
        if (loops > 1 && last_n) {          /* the warm pass alone */
            qsort(last, last_n, sizeof *last, cmp_u64);
            snprintf(line, sizeof line, "[replay] %s: last loop: median %.2f ms, max %.2f ms over %u frames\n",
                     path, last[last_n / 2] / 1e6, last[last_n - 1] / 1e6, last_n);
            fputs(line, stderr);
            if (out) fputs(line, out);
        }
    }
    free(ft);
    return 0;
}
