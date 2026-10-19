/* nv2a_queue.c -- threaded submission in front of any back end
 * (RECOMP_VK_THREAD=1 for the Vulkan renderer).
 *
 * The same scheme as nv2a_gl.c's RECOMP_GL_THREAD queue, made back-end
 * neutral: the executor thread only decodes and copies each clear/draw/flip
 * into a ring, and a render thread of its own replays them into the real
 * back end, so pushbuffer decoding and command recording run on two cores.
 *
 * What a queued draw points at is copied: the method shadow, transform
 * program and constants as the 64/16-byte blocks that differ from what was
 * last queued (the render thread keeps its own copies), vertex data as the
 * byte ranges the draw reads, converted attributes (packed) and indices.
 * Texture images are still read from guest memory when the render thread
 * gets to them, at most two frames later.
 *
 * RECOMP_Q_ZEROCOPY=1: vertex runs in guest memory are not copied either
 * (the copy was ~18% of the executor in gameplay); the render thread reads
 * them where they are. That is safe only because the GPU's completion
 * signals -- the semaphore release and the PATT_COLOR0 progress latch D3D's
 * fence wait polls -- are then queued too and written by the render thread
 * once it has consumed every draw before them: D3D reuses a buffer only
 * after its fence, so nothing a queued draw points at can change under it.
 * DMA_GET still follows the executor's walk (front end vs back end, as on
 * the real chip; deferring GET makes D3D patch already-executed commands). */
#include "nv2a_backend.h"
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { Q_WRAP = 0, Q_DRAW = 1, Q_CLEAR = 2, Q_FLIP = 3, Q_SYNC = 4, Q_POKE = 5 };
/* abs: Q_DRAW attributes whose direct pointer is absolute (zero-copy);
 * Q_POKE: guest VA and value in abs / val. */
typedef struct { uint32_t type, size, nreg, nprog, nconst, abs, val, pad; } QHdr;
typedef struct { uint32_t idx, pad[3]; uint32_t w[16]; } QRegChunk;
typedef struct { uint32_t idx, pad[3]; uint32_t w[4]; } QRow;
typedef struct {
    Nv2aSurface s;
    Nv2aRenderState rs;
    uint32_t flags, argb, zs, pad;
} QClear;
typedef struct { const uint8_t *lo, *hi; } QRun;

#define Q_BYTES   (48u << 20)
#define Q_ALIGN(x) (((x) + 15u) & ~(size_t)15u)
#define REG_WORDS (0x2000 / 4)
#define PROG_ROWS 136
#define CONST_ROWS 192

static const Nv2aBackend *s_real;
static Nv2aBackend s_shim;
static uint8_t *s_q;
static size_t s_q_head, s_q_tail;
static int s_q_consumer_waits, s_q_producer_waits, s_q_flips;
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cv = PTHREAD_COND_INITIALIZER;
static uint64_t s_q_dropped, s_q_idle_ns, s_q_exec_wait_ns, s_q_copy_ns;
static int s_q_all_dirty;           /* no executor (replay): compare every block */
static int s_q_zerocopy;            /* RECOMP_Q_ZEROCOPY=1 */
extern ptrdiff_t xbox_GetMemoryOffset(void);
static volatile int s_q_sync_done, s_q_stop;
static pthread_t s_q_thread;

static uint32_t s_q_regs[REG_WORDS], s_q_prog[PROG_ROWS][4];
static float s_q_consts[CONST_ROWS][4];
static const uint32_t *s_q_live_regs;
static uint32_t s_q_prog_gen, s_q_const_gen;
static uint32_t g_regs[REG_WORDS], g_prog[PROG_ROWS][4];
static float g_consts[CONST_ROWS][4];

static uint64_t q_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void q_timed_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s_q_cv, &s_q_lock, &ts);
}

static size_t q_used(void)
{
    return s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE);
}

static uint8_t *q_reserve(size_t size)
{
    size_t off = s_q_head % Q_BYTES, need = size;
    if (off + size > Q_BYTES)
        need += Q_BYTES - off;
    if (Q_BYTES - q_used() < need) {
        uint64_t t0 = q_now_ns();
        nv2a_pipe_state(0, 3);
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (Q_BYTES - q_used() < need)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        nv2a_pipe_state(0, 0);
        __atomic_add_fetch(&s_q_exec_wait_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
    }
    if (off + size > Q_BYTES) {
        QHdr *w = (QHdr *)(s_q + off);
        w->type = Q_WRAP;
        w->size = (uint32_t)(Q_BYTES - off);
        __atomic_store_n(&s_q_head, s_q_head + (Q_BYTES - off), __ATOMIC_RELEASE);
        off = 0;
    }
    return s_q + off;
}

static void q_publish(size_t size)
{
    __atomic_store_n(&s_q_head, s_q_head + size, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_q_consumer_waits, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&s_q_lock);
        pthread_cond_broadcast(&s_q_cv);
        pthread_mutex_unlock(&s_q_lock);
    }
}

extern uint8_t *nv2a_pb_reg_dirty(void);
static uint16_t s_q_pend[REG_WORDS / 16];
static uint32_t s_q_npend;

static uint32_t q_reg_collect(void)
{
    uint8_t *dirty = s_q_all_dirty ? NULL : nv2a_pb_reg_dirty();
    uint32_t c;
    if (!s_q_live_regs)
        return 0;
    for (c = 0; c < REG_WORDS / 16; c++) {
        if (dirty) {
            if (!dirty[c])
                continue;
            dirty[c] = 0;
        }
        if (memcmp(s_q_live_regs + c * 16, s_q_regs + c * 16, 64) != 0) {
            uint32_t k;
            for (k = 0; k < s_q_npend && s_q_pend[k] != c; k++) { }
            if (k == s_q_npend)
                s_q_pend[s_q_npend++] = (uint16_t)c;
        }
    }
    return s_q_npend;
}

static uint32_t q_reg_emit(QRegChunk *out)
{
    uint32_t k, n = s_q_npend;
    for (k = 0; k < n; k++) {
        uint32_t c = s_q_pend[k];
        out[k].idx = c;
        memcpy(out[k].w, s_q_live_regs + c * 16, 64);
        memcpy(s_q_regs + c * 16, s_q_live_regs + c * 16, 64);
    }
    s_q_npend = 0;
    return n;
}

static uint32_t q_rows_diff(const uint32_t (*live)[4], uint32_t (*sent)[4], uint32_t rows, QRow *out)
{
    uint32_t r, n = 0;
    for (r = 0; r < rows; r++)
        if (memcmp(live[r], sent[r], 16) != 0) {
            if (out) {
                out[n].idx = r;
                memcpy(out[n].w, live[r], 16);
                memcpy(sent[r], live[r], 16);
            }
            n++;
        }
    return n;
}

static size_t q_write_state(uint8_t *rec, uint32_t type, uint32_t nprog, uint32_t nconst,
                            const Nv2aRawBatch *b)
{
    QHdr *h = (QHdr *)rec;
    size_t at = sizeof(QHdr);
    memset(h, 0, sizeof *h);
    h->type = type;
    h->nreg = q_reg_emit((QRegChunk *)(rec + at));
    at += (size_t)h->nreg * sizeof(QRegChunk);
    if (b && nprog) {
        h->nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nprog * sizeof(QRow);
    }
    if (b && nconst) {
        h->nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                                (uint32_t (*)[4])s_q_consts, CONST_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nconst * sizeof(QRow);
    }
    return at;
}

static uint32_t q_elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;
    case 2: return 4 * size;
    case 4: return size;
    case 6: return 4;
    default: return 2 * size;
    }
}

/* The merged byte ranges the direct attributes read. */
static uint32_t q_runs(const Nv2aRawBatch *b, QRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(b->attr_direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + q_elem_bytes(b->direct[a].type, b->direct[a].size);
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(b->attr_direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j) of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1)) of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

static void q_draw(const Nv2aRawBatch *b)
{
    uint64_t t0 = q_now_ns();
    QRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun = b->vertex_count ? q_runs(b, run, of_run) : 0, i, a, nreg, nprog = 0, nconst = 0;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    uint32_t absmask = 0, absrun = 0;
    uint32_t nc = (uint32_t)__builtin_popcount(conv);
    size_t size, at, runs_at[NV2A_RAW_ATTRS], attrs_at = 0, idx_at, batch_at;
    Nv2aRawBatch *qb;
    uint8_t *rec;

    s_q_live_regs = b->regs;
    nreg = q_reg_collect();
    if (b->vp_program && b->vp_prog_gen != s_q_prog_gen && b->vp_slots <= PROG_ROWS)
        nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, NULL);
    if (b->vp_consts && b->vp_const_gen != s_q_const_gen && b->vp_const_count <= CONST_ROWS)
        nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                             (uint32_t (*)[4])s_q_consts, CONST_ROWS, NULL);
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk) + (size_t)(nprog + nconst) * sizeof(QRow);
    batch_at = size;
    size += Q_ALIGN(sizeof(Nv2aRawBatch));
    if (s_q_zerocopy) {
        /* Runs in guest memory stay there (inline arrays live in executor
         * scratch and are still copied). */
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        for (i = 0; i < nrun; i++)
            if (run[i].lo >= mem && run[i].hi <= mem + 0x100000000ull)
                absrun |= 1u << i;
    }
    for (i = 0; i < nrun; i++) {
        if (absrun & (1u << i))
            continue;
        runs_at[i] = size;
        size += Q_ALIGN((size_t)(run[i].hi - run[i].lo) + 4);
    }
    if (conv) {
        attrs_at = size;
        size += Q_ALIGN((size_t)b->vertex_count * nc * 16);
    }
    idx_at = size;
    size += Q_ALIGN((size_t)b->index_count * 4);
    if (size > Q_BYTES / 4) {
        if ((s_q_dropped++ & 1023) == 0)
            fprintf(stderr, "  [queue] draw of %zu bytes dropped (%llu so far)\n",
                    size, (unsigned long long)s_q_dropped);
        return;
    }
    rec = q_reserve(size);
    at = q_write_state(rec, Q_DRAW, nprog, nconst, b);
    s_q_prog_gen = b->vp_prog_gen;
    s_q_const_gen = b->vp_const_gen;
    if (at != batch_at) {
        fprintf(stderr, "  [queue] record mismatch %zu/%zu\n", at, batch_at);
        abort();
    }
    qb = (Nv2aRawBatch *)(rec + batch_at);
    *qb = *b;
    for (i = 0; i < nrun; i++)
        if (!(absrun & (1u << i)))
            memcpy(rec + runs_at[i], run[i].lo, (size_t)(run[i].hi - run[i].lo));
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        if (!(b->attr_direct & (1u << a)))
            continue;
        if (absrun & (1u << of_run[a]))
            absmask |= 1u << a;             /* b->direct[a].ptr as it is */
        else
            qb->direct[a].ptr = (const uint8_t *)(uintptr_t)
                (runs_at[of_run[a]] + (size_t)(b->direct[a].ptr - run[of_run[a]].lo));
    }
    ((QHdr *)rec)->abs = absmask;
    if (conv && b->attrs_packed)
        memcpy(rec + attrs_at, b->attrs, (size_t)b->vertex_count * nc * 16);
    else if (conv) {
        float *out = (float *)(rec + attrs_at);
        uint32_t v, k, n = 0, slot[NV2A_RAW_ATTRS];
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (conv & (1u << a))
                slot[n++] = a;
        for (v = 0; v < b->vertex_count; v++)
            for (k = 0; k < n; k++, out += 4)
                memcpy(out, b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[k]) * 4, 16);
    }
    qb->attrs_packed = conv != 0;
    qb->attrs = (const float *)(uintptr_t)attrs_at;
    memcpy(rec + idx_at, b->indices, (size_t)b->index_count * 4);
    qb->indices = (const uint32_t *)(uintptr_t)idx_at;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
    s_q_copy_ns += q_now_ns() - t0;
}

static void q_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                    uint32_t flags, uint32_t argb, uint32_t zs)
{
    uint32_t nreg = q_reg_collect();
    size_t size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk) + Q_ALIGN(sizeof(QClear));
    uint8_t *rec = q_reserve(size);
    size_t at = q_write_state(rec, Q_CLEAR, 0, 0, NULL);
    QClear *c = (QClear *)(rec + at);
    c->s = *sf;
    c->rs = *rs;
    c->flags = flags;
    c->argb = argb;
    c->zs = zs;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_wait_flips(int max)
{
    if (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) > max) {
        uint64_t t0 = q_now_ns();
        nv2a_pipe_state(0, 3);
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) > max)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        nv2a_pipe_state(0, 0);
        __atomic_add_fetch(&s_q_exec_wait_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
    }
}

static int s_q_max_flips = 2;

static void q_flip(void)
{
    uint32_t nreg;
    size_t size;
    uint8_t *rec;
    q_wait_flips(s_q_max_flips - 1);
    nreg = q_reg_collect();
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk);
    rec = q_reserve(size);
    q_write_state(rec, Q_FLIP, 0, 0, NULL);
    ((QHdr *)rec)->size = (uint32_t)size;
    __atomic_add_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
    q_publish(size);
}

/* A guest word the GPU writes as it completes work (semaphore release,
 * PATT_COLOR0). With RECOMP_Q_ZEROCOPY the render thread writes it after the
 * draws before it; returns 0 when the caller should write it at once. */
int nv2a_queue_poke(uint32_t va, uint32_t val)
{
    QHdr *h;
    if (!s_q) {
        extern int nv2a_gl_queue_poke(uint32_t va, uint32_t val) __attribute__((weak));
        return nv2a_gl_queue_poke ? nv2a_gl_queue_poke(va, val) : 0;
    }
    if (!s_q || !s_q_zerocopy)
        return 0;
    h = (QHdr *)q_reserve(sizeof(QHdr));
    memset(h, 0, sizeof *h);
    h->type = Q_POKE;
    h->size = sizeof(QHdr);
    h->abs = va;
    h->val = val;
    q_publish(sizeof(QHdr));
    return 1;
}

/* Before a trap interrupt: the deferred completion writes land first. */
void nv2a_queue_settle(void)
{
    if (!s_q) {
        extern void nv2a_gl_queue_drain(void) __attribute__((weak));
        if (nv2a_gl_queue_drain) nv2a_gl_queue_drain();
        return;
    }
    if (s_q && s_q_zerocopy) {
        uint64_t t0 = q_now_ns();          /* counted as executor wait */
        nv2a_queue_drain();
        __atomic_add_fetch(&s_q_exec_wait_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
    }
}

/* Blocks until the render thread has replayed everything queued so far. */
void nv2a_queue_drain(void)
{
    uint8_t *rec;
    if (!s_q)
        return;
    s_q_sync_done = 0;
    rec = q_reserve(sizeof(QHdr));
    memset(rec, 0, sizeof(QHdr));
    ((QHdr *)rec)->type = Q_SYNC;
    ((QHdr *)rec)->size = sizeof(QHdr);
    q_publish(sizeof(QHdr));
    pthread_mutex_lock(&s_q_lock);
    s_q_producer_waits = 1;
    while (!__atomic_load_n(&s_q_sync_done, __ATOMIC_ACQUIRE))
        q_timed_wait();
    s_q_producer_waits = 0;
    pthread_mutex_unlock(&s_q_lock);
}

/* Per-frame ms since the last call: render thread idle, executor blocked,
 * executor copying draws. */
void nv2a_queue_stats(uint32_t frames, double *idle, double *wait, double *copy)
{
    double f = frames ? frames : 1;
    *idle = __atomic_exchange_n(&s_q_idle_ns, 0, __ATOMIC_RELAXED) / 1e6 / f;
    *wait = __atomic_exchange_n(&s_q_exec_wait_ns, 0, __ATOMIC_RELAXED) / 1e6 / f;
    *copy = s_q_copy_ns / 1e6 / f;
    s_q_copy_ns = 0;
}

/* Drains the queue and ends the render thread (before the process exits:
 * a live thread at exit crashed hbloader's return). */
void nv2a_queue_shutdown(void)
{
    if (!s_q || s_q_stop)
        return;
    nv2a_queue_drain();
    __atomic_store_n(&s_q_stop, 1, __ATOMIC_RELEASE);
    pthread_join(s_q_thread, NULL);
}

int nv2a_queue_active(void) { return s_q != NULL; }

static void *q_thread(void *arg)
{
    extern void xbox_nx_spread_thread(void) __attribute__((weak));
    (void)arg;
    if (xbox_nx_spread_thread)
        xbox_nx_spread_thread();
    for (;;) {
        size_t avail = __atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) - s_q_tail;
        uint8_t *rec;
        const QHdr *h;
        size_t at;
        uint32_t i;
        int wake = 0;

        if (!avail) {
            uint64_t t0 = q_now_ns();
            if (__atomic_load_n(&s_q_stop, __ATOMIC_ACQUIRE))
                break;
            nv2a_pipe_state(1, 1);
            pthread_mutex_lock(&s_q_lock);
            __atomic_store_n(&s_q_consumer_waits, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) == s_q_tail)
                q_timed_wait();
            __atomic_store_n(&s_q_consumer_waits, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_q_lock);
            nv2a_pipe_state(1, 0);
            __atomic_add_fetch(&s_q_idle_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
            continue;
        }
        rec = s_q + s_q_tail % Q_BYTES;
        h = (const QHdr *)rec;
        if (h->type != Q_WRAP) {
            static uint64_t qdirty[2] = { ~0ull, ~0ull };   /* nv2a_raw_dirty */
            at = sizeof(QHdr);
            for (i = 0; i < h->nreg; i++, at += sizeof(QRegChunk)) {
                const QRegChunk *c = (const QRegChunk *)(rec + at);
                memcpy(g_regs + c->idx * 16, c->w, 64);
                qdirty[(c->idx >> 6) & 1] |= 1ull << (c->idx & 63);
            }
            for (i = 0; i < h->nprog; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_prog[r->idx], r->w, 16);
            }
            for (i = 0; i < h->nconst; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_consts[r->idx], r->w, 16);
            }
            if (h->type == Q_DRAW) {
                Nv2aRawBatch bb = *(const Nv2aRawBatch *)(rec + at);
                uint32_t a;
                bb.regs = g_regs;
                bb.vp_program = (const uint32_t (*)[4])g_prog;
                bb.vp_consts = (const float (*)[4])g_consts;
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if ((bb.attr_direct & (1u << a)) && !(h->abs & (1u << a)))
                        bb.direct[a].ptr = rec + (uintptr_t)bb.direct[a].ptr;
                bb.attrs = (const float *)(rec + (uintptr_t)bb.attrs);
                bb.indices = (const uint32_t *)(rec + (uintptr_t)bb.indices);
                nv2a_raw_dirty = qdirty;
                s_real->draw_raw(&bb);
                nv2a_raw_dirty = NULL;
                qdirty[0] = qdirty[1] = 0;
            } else if (h->type == Q_CLEAR) {
                const QClear *c = (const QClear *)(rec + at);
                s_real->clear(&c->s, &c->rs, c->flags, c->argb, c->zs);
            } else if (h->type == Q_FLIP) {
                s_real->flip();
                __atomic_sub_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
                wake = 1;
            } else if (h->type == Q_POKE) {
                __atomic_store_n((uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + h->abs),
                                 h->val, __ATOMIC_RELEASE);
            } else if (h->type == Q_SYNC) {
                __atomic_store_n(&s_q_sync_done, 1, __ATOMIC_RELEASE);
                wake = 1;
            }
        }
        __atomic_store_n(&s_q_tail, s_q_tail + h->size, __ATOMIC_SEQ_CST);
        if (wake || __atomic_load_n(&s_q_producer_waits, __ATOMIC_SEQ_CST)) {
            pthread_mutex_lock(&s_q_lock);
            pthread_cond_broadcast(&s_q_cv);
            pthread_mutex_unlock(&s_q_lock);
        }
    }
    return NULL;
}

/* `real` behind a render thread when env `var` is 1; otherwise `real`. */
const Nv2aBackend *nv2a_queue_wrap(const Nv2aBackend *real, const char *var)
{
    const char *e = getenv(var), *m = getenv("RECOMP_QUEUE_FLIPS");
    pthread_attr_t at;
    if (!e || *e != '1' || !real->draw_raw)
        return real;
    if (s_q)                            /* install and replay both ask */
        return real == s_real ? &s_shim : real;
    s_q = (uint8_t *)malloc(Q_BYTES + 256);
    if (!s_q)
        return real;
    s_real = real;
    s_q_all_dirty = getenv("RECOMP_REPLAY") != NULL;
    {
        /* Not in a replay: its loop rewrites the captured memory. */
        const char *z = getenv("RECOMP_Q_ZEROCOPY");     /* default on; =0 copies */
        s_q_zerocopy = !(z && *z == '0') && !s_q_all_dirty;
    }
    if (m && *m) s_q_max_flips = atoi(m) > 0 ? atoi(m) : 1;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&s_q_thread, &at, q_thread, NULL) != 0) {
        free(s_q);
        s_q = NULL;
        return real;
    }
    s_shim = *real;
    s_shim.clear = q_clear;
    s_shim.draw = NULL;
    s_shim.flip = q_flip;
    s_shim.draw_raw = q_draw;
    fprintf(stderr, "[queue] render thread on (%s, %d flips ahead%s)\n", var, s_q_max_flips,
            s_q_zerocopy ? ", zero-copy vertices" : "");
    return &s_shim;
}
