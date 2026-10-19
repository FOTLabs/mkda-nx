/*
 * nv2a_vk.c -- a Vulkan 1.3 renderer for the pushbuffer executor.
 *
 * The same job as nv2a_gl.c, on Vulkan: registered as the executor's back end
 * (nv2a_backend.h, draw_raw), it turns the method shadow into draws. Built
 * instead of nv2a_gl for the Switch's NVK (mesa-switch, linked statically)
 * and, on Linux, any Vulkan driver (lavapipe runs it headless).
 *
 *   surfaces   one image per colour-surface address (B8G8R8A8, rows top-first
 *              like guest memory), depth per zeta address (as nv2a_gl). All
 *              images live in VK_IMAGE_LAYOUT_GENERAL: a surface is drawn
 *              into, sampled and blitted without layout changes, ordered by
 *              memory barriers between rendering passes.
 *   textures   the GL renderer's cache and decoders; uploaded through the
 *              frame's staging ring. DXT stays compressed (BC1/2/3).
 *   shaders    gl_vsh.c / gl_psh.c in their Vulkan dialect, compiled to
 *              SPIR-V at run time by glslang, cached by what generated them.
 *   pipelines  keyed by the shader pair, blend state, colour mask, whether
 *              there is a depth buffer and the topology class; everything
 *              else (depth, stencil, cull, winding, topology, vertex layout,
 *              viewport) is dynamic state.
 *   uniforms   two std140 blocks per draw in the frame ring, textures as
 *              combined image samplers, all through push descriptors.
 *
 * Two frames in flight. Everything runs on the executor's thread.
 *
 * Runtime switches (the GL renderer's names where they mean the same):
 *   RECOMP_GL_DUMP=<prefix>[,every]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources on compile errors
 *   RECOMP_GL_SCALE=<k>              render surfaces at k times the title's
 *                                    resolution, fractions allowed (1.5;
 *                                    0.5..4, default 1)
 *   RECOMP_VK_VALIDATION=1           (Linux) enable the Khronos validation layer
 *   RECOMP_VK_HEADLESS=1             (Linux) no window, no present
 */
#include "nv2a_vk.h"
#include "../nv2a_gl/gl_psh.h"
#include "../nv2a_gl/gl_vsh.h"
#include "../kernel/nv2a_backend.h"

#if defined(__SWITCH__)
#include <switch.h>
#define VK_USE_PLATFORM_VI_NN 1
#endif
#include <vulkan/vulkan.h>
#if !defined(__SWITCH__)
#include <SDL.h>
#include <SDL_vulkan.h>
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define LOGE(...) fprintf(stderr, "  [VK] " __VA_ARGS__)

static int s_state;            /* 0 untried, 1 ready, -1 failed */
static int s_trace;
/* RECOMP_VK_TRACE=<n>: name every step of the first n draws, clears and
 * flips (a crash inside the driver then ends the log at the step). */
static int s_vtrace;
#define VT(...) do { if (s_vtrace > 0) { fprintf(stderr, "  [VKT] " __VA_ARGS__); fflush(stderr); } } while (0)
static uint32_t s_frame;
static uint64_t s_nverts, s_nidx, s_nspan, s_nprim[16], s_nuniq, s_ndegen;
/* RECOMP_VK_VPSTATS=1: per transform program (by content): draws, indices,
 * unique vertices, instruction slots, attributes present. */
#define VPS_N 128
static struct { uint64_t h; uint32_t slots, attrs; uint64_t draws, idx, uniq; } s_vps[VPS_N];
static uint32_t s_nvps;
static int s_vps_on = -1;
static uint64_t s_last_uniq;   /* per report: gathered vertices, indices drawn */
static uint32_t s_passes, s_passes_ms, s_uploads;   /* per frame: passes, MSAA ones, texture uploads */
static uint64_t s_clr_ns[6]; /* vk_clear: target, begin, cmd; ready, surf_get, depth_get */
static uint64_t s_flp_ns[4];                        /* flip: submit, present, fence wait */
static int s_drew_any;
static const uint32_t *s_regs;
static const Nv2aRawBatch *s_batch;

/* Hitch accounting, as in nv2a_gl. */
static uint32_t s_hz_progs, s_hz_texs, s_hz_draws;
static uint64_t s_hz_prog_ns, s_hz_tex_ns, s_hz_tex_bytes;
static uint64_t hz_now(void)
{
#if defined(__SWITCH__)
    /* The system counter (19.2 MHz): clock_gettime there is coarse enough
     * that short sections right after the frame fence's wake-up all read as
     * one tick (clears showed ~1 ms each with nothing behind them). */
    uint64_t t;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t * 625 / 12;                        /* x 1e9 / 19.2e6 */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ── Device ───────────────────────────────────────────────────────── */

static VkInstance       s_inst;
static VkPhysicalDevice s_pd;
static VkDevice         s_dev;
static VkQueue          s_queue;
static uint32_t         s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkDeviceSize     s_ubo_align = 256;
static VkFormat         s_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
static int              s_have_bc, s_have_depth_clamp;

static PFN_vkCmdPushDescriptorSetKHR  p_push_desc;
/* RECOMP_VK_EXP: GPU cost experiments, replay only (images go wrong).
 * 1 descriptors pushed once per pass, 2 pipeline bound once per command
 * buffer, 4 vertex input set once per command buffer, 8 no draw calls,
 * 16 dynamic state applied once per pass. */
static int s_exp = -1;
static PFN_vkCmdSetVertexInputEXT     p_vertex_input;
static PFN_vkCmdBeginRendering        p_begin_rendering;
static PFN_vkCmdEndRendering          p_end_rendering;
static PFN_vkCmdSetCullMode           p_cull_mode;
static PFN_vkCmdSetFrontFace          p_front_face;
static PFN_vkCmdSetPrimitiveTopology  p_topology;
static PFN_vkCmdSetDepthTestEnable    p_depth_test;
static PFN_vkCmdSetDepthWriteEnable   p_depth_write;
static PFN_vkCmdSetDepthCompareOp     p_depth_op;
static PFN_vkCmdSetStencilTestEnable  p_stencil_test;
static PFN_vkCmdSetStencilOp          p_stencil_op;

/* Presentation. */
static VkSurfaceKHR   s_surface;
static VkSwapchainKHR s_swapchain;
static VkFormat       s_sc_format;
static VkExtent2D     s_sc_extent;
static uint32_t       s_sc_count;
static VkImage        s_sc_images[8];
static VkImageView    s_sc_views[8];
static int            s_headless;
#if !defined(__SWITCH__)
static SDL_Window    *s_win;
#endif

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements rq, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m = VK_NULL_HANDLE;
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return VK_NULL_HANDLE;
    if (vkAllocateMemory(s_dev, &ai, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

/* ── Frames in flight ─────────────────────────────────────────────── */

#define VK_FRAMES 2
#define RING_BYTES (48u << 20)
/* Indices go to a ring of their own, written once as u32: a merged draw's
 * indices must stay contiguous while its vertices go into the main ring,
 * and the u32 -> u16 loop was ~10% of vk_draw_raw. */
#define IRING_BYTES (16u << 20)

typedef struct {
    VkImage image; VkImageView view; VkDeviceMemory mem; VkPipeline pipe;
} Garbage;

typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkCommandBuffer pcb;            /* the present work (split_present) */
    VkFence         fence;
    VkSemaphore     acquired, rendered;
    VkBuffer        ring;
    VkDeviceMemory  ring_mem;
    uint8_t        *ring_ptr;
    VkDeviceSize    ring_off;
    VkBuffer        iring;          /* this frame's indices, u32 (IRING_BYTES) */
    VkDeviceMemory  iring_mem;
    uint8_t        *iring_ptr;
    VkDeviceSize    ioff;
    int             submitted;
    Garbage        *garbage;
    uint32_t        ngarbage, garbage_cap;
} Frame;

static Frame  s_fr[VK_FRAMES];
static Frame *s_f;             /* the frame being recorded */
static uint32_t s_fi;
static VkCommandBuffer s_cb;   /* s_f->cb while recording */
static uint32_t s_cb_gen;      /* bumped at every vkBeginCommandBuffer */
static int s_dyn_valid;

static void garbage_add_pipe(VkPipeline pipe);
/* Bumped whenever a texture or surface image is made or freed: a stage
 * binding cached by bind_stage is valid only within one epoch. */
static uint32_t s_bind_epoch;
static uint32_t s_post_va;      /* RECOMP_VK_POSTLOG: last surface resolved for sampling */
/* repetition counters (per frame in nv2a_vk_cpu_stats): vertex-input sets,
 * vertex-buffer binds, run hits/misses, bytes copied, bytes converted,
 * pipeline binds, uniform block writes (vs, fs), dynamic-offset binds, image
 * pushes, stage-cache hits */
static uint64_t s_rep[12];
static uint64_t s_ms_n, s_ms_all, s_ms_noconst, s_ms_regs, s_ms_k;
static uint64_t s_merged;           /* RECOMP_VK_MERGE: draws joined to another */
static uint64_t s_jn[5], s_jrej[16], s_jnopend, s_srej[8];            /* join: fields, regs, runs, copy (bytes in [4]) */   /* RECOMP_VK_MERGESTATS */
static void garbage_add(VkImage img, VkImageView view, VkDeviceMemory mem)
{
    Frame *f = s_f;
    s_bind_epoch++;
    if (f->ngarbage == f->garbage_cap) {
        f->garbage_cap = f->garbage_cap ? f->garbage_cap * 2 : 64;
        f->garbage = (Garbage *)realloc(f->garbage, f->garbage_cap * sizeof *f->garbage);
    }
    f->garbage[f->ngarbage].image = img;
    f->garbage[f->ngarbage].view = view;
    f->garbage[f->ngarbage].mem = mem;
    f->garbage[f->ngarbage].pipe = VK_NULL_HANDLE;
    f->ngarbage++;
}

static void garbage_add_pipe(VkPipeline pipe)
{
    garbage_add(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    s_f->garbage[s_f->ngarbage - 1].pipe = pipe;
}

static void garbage_free(Frame *f)
{
    uint32_t i;
    for (i = 0; i < f->ngarbage; i++) {
        if (f->garbage[i].view) vkDestroyImageView(s_dev, f->garbage[i].view, NULL);
        if (f->garbage[i].image) vkDestroyImage(s_dev, f->garbage[i].image, NULL);
        if (f->garbage[i].mem) vkFreeMemory(s_dev, f->garbage[i].mem, NULL);
        if (f->garbage[i].pipe) vkDestroyPipeline(s_dev, f->garbage[i].pipe, NULL);
    }
    f->ngarbage = 0;
}

/* GPU time per frame: a timestamp at the top of each frame's command buffer
 * and one before its submit (query pool of 2 per frame slot), read back once
 * the slot's fence has passed. nv2a_vk_gpu_stats() reports and resets them. */
static VkQueryPool s_tq;
static double      s_tq_period_ns;
static int         s_tq_valid[8];
static uint64_t    s_gpu_ns_sum, s_gpu_ns_max;
static uint32_t    s_gpu_n;

int nv2a_vk_gpu_stats(double *avg_ms, double *max_ms)
{
    uint32_t n = s_gpu_n;
    if (avg_ms) *avg_ms = n ? s_gpu_ns_sum / 1e6 / n : 0;
    if (max_ms) *max_ms = s_gpu_ns_max / 1e6;
    s_gpu_ns_sum = s_gpu_ns_max = 0;
    s_gpu_n = 0;
    return (int)n;
}

/* RECOMP_VK_FTRACE=1: a per-frame timeline (CPU clock, hz_now) printed when
 * the frame's GPU timestamps come back -- where CPU or GPU sits idle. Frame
 * f is the one recorded while s_frame == f. */
#define FT_N 64
typedef struct { uint64_t start, rec_end, submit, flip_end, wait, g0, g1; } FTrace;
static FTrace   s_ft[FT_N];
static uint32_t s_fr_frame[8];
static int      s_ft_on = -1, s_ft_open;
static uint32_t s_ft_draws[FT_N];
static void ft_secs(uint32_t frame, int end);       /* per-frame section times */
static void ft_secs_print(uint32_t frame, double rec_ms);
static void ft_start(uint32_t frame)
{
    if (s_ft_on < 0) { const char *e = getenv("RECOMP_VK_FTRACE"); s_ft_on = e && *e == '1'; }
    if (s_ft_on && !s_ft_open) {
        s_ft_open = 1;
        s_ft[frame % FT_N].start = hz_now();
        s_ft_draws[frame % FT_N] = 0;
        ft_secs(frame, 0);
    }
}
static void ft_print(uint32_t f)
{
    FTrace *a = &s_ft[f % FT_N], *n = &s_ft[(f + 1) % FT_N], *p = &s_ft[(f - 1) % FT_N];
    double per = s_tq_period_ns;
    fprintf(stderr, "[ftrace] f%u rec %.2f submit %.2f flip-rest %.2f | producer %.2f slot-wait %.2f | "
            "gpu %.2f gpu-idle-before %.2f | cpu frame %.2f\n", f,
            (a->rec_end - a->start) / 1e6, (a->submit - a->rec_end) / 1e6, (a->flip_end - a->submit) / 1e6,
            n->start > a->flip_end ? (n->start - a->flip_end) / 1e6 : -1.0, a->wait / 1e6,
            (a->g1 - a->g0) * per / 1e6, p->g1 && a->g0 > p->g1 ? (a->g0 - p->g1) * per / 1e6 : -1.0,
            n->start > a->start ? (n->start - a->start) / 1e6 : -1.0);
    ft_secs_print(f, (a->rec_end - a->start) / 1e6);
}

static void pp_collect(uint64_t frame_ns);
static void pp_frame_start(int bad);

static void frame_begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    s_f = &s_fr[s_fi];
    if (s_f->submitted) {
        uint64_t tw = hz_now();
        nv2a_pipe_state(1, 2);
        vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
        nv2a_pipe_state(1, 0);
        s_flp_ns[2] += hz_now() - tw;
        if (s_ft_on > 0) s_ft[s_frame % FT_N].wait = hz_now() - tw;
        vkResetFences(s_dev, 1, &s_f->fence);
        s_f->submitted = 0;
        if (s_tq && s_tq_valid[s_fi]) {
            uint64_t t[2];
            if (vkGetQueryPoolResults(s_dev, s_tq, (uint32_t)s_fi * 2, 2, sizeof t, t, 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * s_tq_period_ns);
                if (s_ft_on > 0) {
                    s_ft[s_fr_frame[s_fi] % FT_N].g0 = t[0];
                    s_ft[s_fr_frame[s_fi] % FT_N].g1 = t[1];
                    ft_print(s_fr_frame[s_fi]);
                }
                s_gpu_ns_sum += ns;
                if (ns > s_gpu_ns_max) s_gpu_ns_max = ns;
                s_gpu_n++;
                pp_collect(ns);
            }
        }
    }
    s_tq_valid[s_fi] = 0;
    garbage_free(s_f);
    s_f->ring_off = 0;
    s_f->ioff = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_f->cb, &bi);
    s_cb = s_f->cb;
    s_cb_gen++;
    s_dyn_valid = 0;
    if (s_tq) {
        vkCmdResetQueryPool(s_cb, s_tq, (uint32_t)s_fi * 2, 2);
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tq, (uint32_t)s_fi * 2);
    }
    pp_frame_start(0);
}

/* Space in this frame's ring: returns the offset, *ptr the mapping. */
static void flush_frame_wait(void);
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **ptr)
{
    VkDeviceSize at = (s_f->ring_off + align - 1) & ~(align - 1);
    if (at + size > RING_BYTES) {
        /* A frame larger than the ring: finish what is recorded, wait for
         * it, and start over in the same ring. Slow, but only in extremes. */
        static int warned;
        if (!warned++)
            LOGE("frame ring (%u MB) full: flushing mid-frame\n", RING_BYTES >> 20);
        flush_frame_wait();
        at = 0;
        if (size > RING_BYTES) {
            *ptr = NULL;
            return 0;
        }
    }
    s_f->ring_off = at + size;
    *ptr = s_f->ring_ptr + at;
    return at;
}

/* ── Rendering passes and barriers ────────────────────────────────── */

/* w, h are the surface in the title's pixels (anti-aliasing included) --
 * what the shaders, clip rectangles and texture lookups work in. pw, ph are
 * the image: w, h times the render scale. */
typedef struct {
    uint32_t va, w, h, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkImageView view_a1;       /* alpha swizzled to 1, for alpha-less formats */
    VkImage ms_image; VkImageView ms_view; VkDeviceMemory ms_mem;   /* s_msaa > 1 */
    int ms_dirty;               /* the twin has samples the image has not seen */
    int ss_newer;               /* the image has drawing the twin has not (ss_post) */
    uint32_t used;
    uint32_t aa_sx, aa_sy;
    int dirty;                 /* written since the last barrier */
} VkSurf;
static void surf_readback(VkSurf *s, void (*fn)(const uint8_t *, VkSurf *, void *), void *ctx);
static void probe_count(const uint8_t *p, VkSurf *s, void *ctx);

typedef struct {
    uint32_t va, pw, ph, samples;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
} VkDepthBuf;

#define VK_MAX_SURF 128   /* Conker: ~40 a frame (32x32 lighting targets per character); 32 thrashed */
#define VK_MAX_DEPTH 16

/* RECOMP_GL_SCALE, as in nv2a_gl: only the pixels behind a surface grow, so
 * the viewport, clear rectangles, read-backs and the present blit scale and
 * nothing else does. Capped per surface by the device's image limits. */
static double   s_scale = 1.0;
/* RECOMP_GL_MSAA=<samples> (or the platform default, as nv2a_gl.c): every
 * surface draws into a multisampled twin (VkSurf.ms_*) that each rendering
 * pass resolves into the single-sample image -- the one sampled, read back
 * and presented, so nothing else changes. Depth buffers are multisampled. */
static VkSampleCountFlagBits s_msaa = VK_SAMPLE_COUNT_1_BIT;
static VkSampleCountFlagBits s_img_samples = VK_SAMPLE_COUNT_1_BIT;   /* make_image_ex */
static uint32_t s_img_levels = 1;                                        /* make_image_ex */
static int      s_have_aniso;
extern double xbox_platform_gl_scale(void) __attribute__((weak));
extern int    xbox_platform_gl_msaa(void) __attribute__((weak));
static int      s_have_large_points;
static uint32_t s_max_size = 4096;

/* v title pixels of a surface l wide, in the p stored pixels behind it. */
static uint32_t to_stored(uint32_t v, uint32_t p, uint32_t l)
{
    return (uint32_t)(((uint64_t)v * p + l / 2) / l);
}
static VkSurf     s_surf[VK_MAX_SURF];
static VkDepthBuf s_depth[VK_MAX_DEPTH];
static VkSurf    *s_last;

static int         s_in_render;
static VkSurf     *s_rt;
static VkDepthBuf *s_rt_depth;

static void barrier_all(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    int i;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (i = 0; i < VK_MAX_SURF; i++)
        s_surf[i].dirty = 0;
}

/* RECOMP_VK_PASSPROF=1: GPU time of every rendering pass and resolve, by
 * timestamps around each (passes are serialised by barrier_all, so the
 * difference is the pass). Aggregated per target over the frames since the
 * last nv2a_vk_pass_report(). */
#define PP_MAX 768
typedef struct { uint8_t kind, samples; uint16_t draws, clears; uint32_t va, pw, ph, tris; } PPSeg;
static int         s_pp_on = -1;
static VkQueryPool s_pp;
static PPSeg       s_pp_seg[VK_FRAMES][PP_MAX];
static uint32_t    s_pp_n[VK_FRAMES];
static int         s_pp_bad[VK_FRAMES];
static PPSeg      *s_pp_cur;
#define PP_AGG 256
typedef struct { uint8_t kind, samples; uint32_t va, pw, ph; uint64_t ns, n, draws, clears, tris; } PPAgg;
static PPAgg       s_pp_agg[PP_AGG];
static uint32_t    s_pp_nagg, s_pp_frames;
static uint64_t    s_pp_frame_ns;

static void pp_init(void)
{
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    const char *e = getenv("RECOMP_VK_PASSPROF");
    s_pp_on = e && *e == '1' && s_tq_period_ns > 0;
    if (!s_pp_on)
        return;
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = VK_FRAMES * PP_MAX * 2;
    if (vkCreateQueryPool(s_dev, &qpi, NULL, &s_pp) != VK_SUCCESS)
        s_pp_on = 0;
}

/* Recorded outside rendering, at the start of a slot's command buffer. */
static void pp_frame_start(int bad)
{
    if (s_pp_on <= 0)
        return;
    vkCmdResetQueryPool(s_cb, s_pp, (uint32_t)s_fi * PP_MAX * 2, PP_MAX * 2);
    s_pp_n[s_fi] = 0;
    s_pp_bad[s_fi] = bad;
    s_pp_cur = NULL;
}

static void pp_begin(int kind, const VkSurf *s, int samples)
{
    PPSeg *g;
    if (s_pp_on <= 0 || s_pp_n[s_fi] >= PP_MAX)
        return;
    g = &s_pp_seg[s_fi][s_pp_n[s_fi]];
    memset(g, 0, sizeof *g);
    g->kind = (uint8_t)kind;
    g->samples = (uint8_t)samples;
    g->va = s->va; g->pw = s->pw; g->ph = s->ph;
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pp, ((uint32_t)s_fi * PP_MAX + s_pp_n[s_fi]) * 2);
    s_pp_cur = g;
}

static void pp_end(void)
{
    if (!s_pp_cur)
        return;
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_pp, ((uint32_t)s_fi * PP_MAX + s_pp_n[s_fi]) * 2 + 1);
    s_pp_n[s_fi]++;
    s_pp_cur = NULL;
}

/* After the slot's fence: fold its segments into the aggregate. */
static void pp_collect(uint64_t frame_ns)
{
    static uint64_t t[PP_MAX * 2];
    uint32_t n = s_pp_n[s_fi], i, k;
    if (s_pp_on <= 0 || !n || s_pp_bad[s_fi])
        return;
    if (vkGetQueryPoolResults(s_dev, s_pp, (uint32_t)s_fi * PP_MAX * 2, n * 2, n * 16, t, 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (i = 0; i < n; i++) {
        const PPSeg *g = &s_pp_seg[s_fi][i];
        uint64_t ns = t[2 * i + 1] > t[2 * i] ? (uint64_t)((t[2 * i + 1] - t[2 * i]) * s_tq_period_ns) : 0;
        for (k = 0; k < s_pp_nagg; k++)
            if (s_pp_agg[k].kind == g->kind && s_pp_agg[k].va == g->va && s_pp_agg[k].pw == g->pw
                && s_pp_agg[k].ph == g->ph && s_pp_agg[k].samples == g->samples)
                break;
        if (k == s_pp_nagg) {
            if (k == PP_AGG) continue;
            memset(&s_pp_agg[k], 0, sizeof s_pp_agg[k]);
            s_pp_agg[k].kind = g->kind; s_pp_agg[k].va = g->va; s_pp_agg[k].pw = g->pw;
            s_pp_agg[k].ph = g->ph; s_pp_agg[k].samples = g->samples;
            s_pp_nagg++;
        }
        s_pp_agg[k].ns += ns;
        s_pp_agg[k].n++;
        s_pp_agg[k].draws += g->draws;
        s_pp_agg[k].clears += g->clears;
        s_pp_agg[k].tris += g->tris;
    }
    s_pp_frames++;
    s_pp_frame_ns += frame_ns;
}

static int pp_cmp(const void *a, const void *b)
{
    const PPAgg *x = (const PPAgg *)a, *y = (const PPAgg *)b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

/* Prints the per-target table to stderr (and out) and resets; out NULL and
 * print 0 only resets. */
void nv2a_vk_pass_report(FILE *out, int print)
{
    uint32_t k;
    double f = s_pp_frames ? s_pp_frames : 1, sum = 0, by[6] = { 0 };
    char line[256];
    if (s_pp_on > 0 && print && s_pp_frames) {
        qsort(s_pp_agg, s_pp_nagg, sizeof *s_pp_agg, pp_cmp);
        snprintf(line, sizeof line, "[passprof] %u frames, GPU frame %.2f ms; per frame by target (ms, count, draws, clears, ktris):\n",
                 s_pp_frames, s_pp_frame_ns / 1e6 / f);
        fputs(line, stderr); if (out) fputs(line, out);
        for (k = 0; k < s_pp_nagg; k++) {
            const PPAgg *a = &s_pp_agg[k];
            double ms = a->ns / 1e6 / f;
            int big = a->pw * a->ph >= 300000, tiny = a->pw * a->ph <= 64 * 64;
            sum += ms;
            if (a->kind == 2) by[3] += ms;
            else if (big) by[0] += ms;
            else if (tiny) by[2] += ms;
            else by[1] += ms;
            if (a->kind == 1 && a->samples > 1) by[4] += ms;
            if (k < 30) {
                snprintf(line, sizeof line, "[passprof]  %-7s %08X %4ux%-4u x%u  %6.2f ms  n %5.1f  draws %6.1f  clears %4.1f  ktris %6.1f\n",
                         a->kind == 2 ? "resolve" : "pass", a->va, a->pw, a->ph, a->samples, ms,
                         a->n / f, a->draws / f, a->clears / f, a->tris / 1e3 / f);
                fputs(line, stderr); if (out) fputs(line, out);
            }
        }
        snprintf(line, sizeof line, "[passprof] buckets: full-size passes %.2f, mid passes %.2f, tiny (<=64x64) passes %.2f, "
                 "resolves %.2f; MSAA passes %.2f; outside passes (uploads, copies, barriers) %.2f ms\n",
                 by[0], by[1], by[2], by[3], by[4], s_pp_frame_ns / 1e6 / f - sum);
        fputs(line, stderr); if (out) fputs(line, out);
    }
    s_pp_nagg = 0;
    s_pp_frames = 0;
    s_pp_frame_ns = 0;
}

static void pd_flush(void);
static void end_rendering(void)
{
    pd_flush();
    if (s_in_render) {
        p_end_rendering(s_cb);
        s_in_render = 0;
        pp_end();
    }
}

static VkRect2D s_sc_cur;               /* scissor set in this pass */

static void set_viewport(uint32_t pw, uint32_t ph)
{
    VkViewport vp = { 0, 0, (float)pw, (float)ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { pw, ph } };
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    s_sc_cur = sc;
}

/* SET_SURFACE_CLIP as the scissor, in s's stored pixels. NFSU2's split
 * screen draws each player's world with the clip set to that player's
 * half; the vertex programs place it there but do not stop at the edge.
 * Same as nv2a_gl.c (and xemu). Call inside the pass (begin_rendering
 * resets it to the whole surface). */
static void set_surface_scissor(const uint32_t *r, const VkSurf *s)
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;
    VkRect2D sc = { { 0, 0 }, { s->pw, s->ph } };

    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 > x0 && y1 > y0) {
        sc.offset.x = (int32_t)to_stored(x0, s->pw, s->w);
        sc.offset.y = (int32_t)to_stored(y0, s->ph, s->h);
        sc.extent.width = to_stored(x1, s->pw, s->w) - (uint32_t)sc.offset.x;
        sc.extent.height = to_stored(y1, s->ph, s->h) - (uint32_t)sc.offset.y;
    }
    if (memcmp(&sc, &s_sc_cur, sizeof sc)) {
        vkCmdSetScissor(s_cb, 0, 1, &sc);
        s_sc_cur = sc;
    }
}

/* RECOMP_VK_SS_POST=1 (default off: the copy-back runs every frame in barn, net -0.2 ms GPU): a multisampled surface whose image is up
 * to date (resolved, nothing drawn since) takes draws with no depth or
 * stencil in the image itself -- the full-screen post passes at the end of
 * a frame (barn: one glow-composite quad after the mid-frame resolve for the
 * downsample). Its flip then needs no second full-surface resolve. A later
 * multisampled draw copies the image back into every sample first
 * (ss_unresolve; edges drawn before lose their anti-aliasing then, which
 * only a depth-tested draw after such a pass causes). s_want_ss: the next
 * begin_rendering is such a draw (vk_draw_raw). */
static int s_want_ss, s_rt_ss;
static void ss_unresolve(VkSurf *s);

static void begin_rendering(VkSurf *s, VkDepthBuf *d)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    int ss = s_want_ss && s->ms_view && !s->ms_dirty;

    s_want_ss = 0;
    if (ss)
        d = NULL;
    if (s_in_render && s_rt == s && s_rt_depth == d && s_rt_ss == ss)
        return;
    end_rendering();
    barrier_all();
    if (s->ms_view && !ss && s->ss_newer) {
        ss_unresolve(s);                /* the twin catches up first */
        barrier_all();
    }
    ca.imageView = s->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (ss) {
        s->ss_newer = 1;
    } else if (s->ms_view) {    /* s_msaa: draw into the twin; surf_resolve averages it on demand */
        ca.imageView = s->ms_view;
        s->ms_dirty = 1;
    }
    s_rt_ss = ss;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    if (d) {
        if (d->pw < ri.renderArea.extent.width) ri.renderArea.extent.width = d->pw;
        if (d->ph < ri.renderArea.extent.height) ri.renderArea.extent.height = d->ph;
    }
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    p_begin_rendering(s_cb, &ri);
    pp_begin(1, s, s->ms_view && !ss ? (int)s_msaa : 1);
    s_passes++;
    if (s->ms_view && !ss) s_passes_ms++;
    s_in_render = 1;
    s_rt = s;
    s_rt_depth = d;
    set_viewport(s->pw, s->ph);
}

/* s_msaa: bring the multisampled drawing into the surface's image before
 * anything reads it (sampling, present, read-back). Passes used to resolve at
 * every end -- 27 full-surface resolves a frame in the barn fixture, the main
 * surface again after every lighting-target switch. */
static void surf_resolve_at(VkSurf *x, int line);
static int shader_resolve(VkSurf *x);
#define surf_resolve(x) surf_resolve_at((x), __LINE__)
static void surf_resolve_at(VkSurf *x, int line)
{
    VkImageResolve rg;
    if (!x || !x->ms_image || !x->ms_dirty)
        return;
    if (getenv("RECOMP_VK_RESLOG")) {
        static uint32_t nlog;
        if (nlog++ < 40)
            fprintf(stderr, "[reslog] frame %u: resolve %08X from line %d\n", s_frame, x->va, line);
    }
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.srcSubresource.layerCount = 1;
    rg.dstSubresource = rg.srcSubresource;
    rg.extent.width = x->pw;
    rg.extent.height = x->ph;
    rg.extent.depth = 1;
    pp_begin(2, x, (int)s_msaa);
    if (!shader_resolve(x))
        vkCmdResolveImage(s_cb, x->ms_image, VK_IMAGE_LAYOUT_GENERAL, x->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    pp_end();
    barrier_all();
    x->ms_dirty = 0;
}

/* An image in GENERAL layout, from UNDEFINED; recorded outside rendering. */
static void image_to_general(VkImage img, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    end_rendering();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* cube: six layers, cube-compatible, viewed as a cube */
static int make_image_ex(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, int cube, VkImage *img, VkImageView *view,
                         VkDeviceMemory *mem)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements rq;
    {   /* RECOMP_VK_IMGLOG=1 (diagnostic): every image created, with the frame */
        static int lg = -1;
        if (lg < 0) { const char *e = getenv("RECOMP_VK_IMGLOG"); lg = e && *e == '1'; }
        if (lg)
            fprintf(stderr, "[img] frame %u: %ux%u fmt %d samples %d levels %u cube %d usage %X\n",
                    s_frame, w, h, (int)fmt, (int)s_img_samples, s_img_levels, cube, (unsigned)usage);
    }

    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = s_img_levels;
    ci.arrayLayers = cube ? 6 : 1;
    if (cube)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.samples = s_img_samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, *img, &rq);
    *mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!*mem || vkBindImageMemory(s_dev, *img, *mem, 0) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        if (*mem) vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    vi.image = *img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = s_img_levels;
    vi.subresourceRange.layerCount = cube ? 6 : 1;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        vkFreeMemory(s_dev, *mem, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        return 0;
    }
    image_to_general(*img, aspect);
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkImageView *view,
                      VkDeviceMemory *mem)
{
    return make_image_ex(w, h, fmt, usage, aspect, 0, img, view, mem);
}

/* ── Surfaces ─────────────────────────────────────────────────────── */

static VkSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

static void clear_image_color(VkImage img, float r, float g, float b, float a)
{
    VkClearColorValue c = { { r, g, b, a } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
    end_rendering();
    vkCmdClearColorImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
}

/* Which surfaces get the multisampled twin: RECOMP_VK_MSAA_MIN=<title
 * pixels> (default 200000: the 640x480 back buffers, not the 48..384 px
 * lighting maps and post-process targets -- full-surface quads and tiny
 * light maps show no edges; their 4x passes and resolves were ~4 ms of the
 * barn frame). 0: every surface, as before. */
static int surf_msaa(uint32_t w, uint32_t h)
{
    static long min_px = -1;
    if (min_px < 0) {
        const char *e = getenv("RECOMP_VK_MSAA_MIN");
        min_px = e && *e ? atol(e) : 200000;
    }
    return (long)w * (long)h >= min_px;
}

/* The registers of the draw whose target is being looked up (NULL for a
 * clear): a surface created for a draw starts with its guest memory. */
static const uint32_t *s_target_regs;

/* Render-target losses (Switch perf report, nv2a_vk_rt_report): a surface
 * evicted from the table, and a texture later bound from guest memory at an
 * address that was a render target. Rendered contents never go back to guest
 * memory here, so such a bind reads whatever the CPU left there -- for
 * Conker's blob-shadow pool, zeros, i.e. no shadow that frame. */
static uint32_t s_rt_evicts, s_rt_lost_binds, s_rt_gone[256], s_rt_gone_n;

void nv2a_vk_rt_report(double secs)
{
    (void)secs;
    fprintf(stderr, "[perf] RT: %u surfaces evicted, %u binds of an evicted target from memory\n",
            s_rt_evicts, s_rt_lost_binds);
    s_rt_evicts = s_rt_lost_binds = 0;
}
static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h);
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **ptr);
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* A render target on the NV2A is guest memory, so a draw into one nobody
 * cleared blends onto what is there. Conker never clears its pool of 32x32
 * blob-shadow targets (81D90000..81D96300): it draws each over the previous
 * contents, white where there is no shadow. Created black here, the shadow
 * multiplied the floor and haystacks to black on every frame that used a
 * pool entry rendered in this run -- alternate frames, as the game cycles
 * through the pool (fixtures/shadowflk.fixz). So a surface created for a
 * draw is filled from its memory (A8R8G8B8 / X8R8G8B8 / R5G6B5, pitch or
 * swizzled), scaled to the stored size. Multisampled (large) surfaces keep
 * the clear: they are the back buffers, which the title clears. */
static void surf_init_from_memory(VkSurf *s, uint32_t w, uint32_t h)
{
    const uint32_t *r = s_target_regs;
    uint32_t fmtreg, cfmt, type, pitch, bpp, x, y;
    const uint8_t *mem;
    uint32_t *px;
    void *dst;
    VkDeviceSize at;
    VkImage tmp;
    VkImageView tview;
    VkDeviceMemory tmem;
    VkBufferImageCopy rg;
    VkImageBlit bl;

    if (getenv("RECOMP_SURFINIT_LOG"))
        fprintf(stderr, "[surfinit] %08X %ux%u regs %d fmt %08X pitch %08X ms %d\n", s->va, w, h,
                r != NULL, r ? r[0x208 / 4] : 0, r ? r[0x20C / 4] : 0, s->ms_image != VK_NULL_HANDLE);
    if (!r || s->ms_image || !w || !h || w > 2048 || h > 2048)
        return;
    fmtreg = r[0x208 / 4];
    cfmt = fmtreg & 0xF;
    type = (fmtreg >> 8) & 0xF;
    pitch = r[0x20C / 4] & 0xFFFF;
    if (cfmt == 0x3) bpp = 2;                               /* R5G6B5 */
    else if (cfmt == 0x8 || cfmt == 0x4 || cfmt == 0x5) bpp = 4;   /* A8R8G8B8, X8R8G8B8 */
    else return;
    if (type == 2 && ((w & (w - 1)) || (h & (h - 1))))
        return;                                             /* swizzle needs powers of two */
    if (type != 2 && pitch < w * bpp)
        return;
    at = ring_alloc((VkDeviceSize)w * h * 4, 16, &dst);
    if (!dst)
        return;
    mem = (const uint8_t *)xbox_GetMemoryOffset() + s->va;
    px = (uint32_t *)dst;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            size_t off = type == 2 ? (size_t)swizzle_index(x, y, w, h) * bpp
                                   : (size_t)y * pitch + (size_t)x * bpp;
            uint32_t c;
            if (bpp == 4) {
                memcpy(&c, mem + off, 4);
                if (cfmt != 0x8) c |= 0xFF000000u;
            } else {
                uint16_t v;
                memcpy(&v, mem + off, 2);
                c = 0xFF000000u | ((uint32_t)((v >> 11) * 255 / 31) << 16)
                  | ((uint32_t)(((v >> 5) & 63) * 255 / 63) << 8) | (uint32_t)((v & 31) * 255 / 31);
            }
            px[(size_t)y * w + x] = c;                      /* B G R A bytes */
        }
    if (getenv("RECOMP_SURFINIT_LOG")) {
        uint64_t sum[4] = {0, 0, 0, 0};
        uint32_t i;
        for (i = 0; i < w * h; i++) {
            sum[0] += px[i] & 0xFF; sum[1] += (px[i] >> 8) & 0xFF;
            sum[2] += (px[i] >> 16) & 0xFF; sum[3] += px[i] >> 24;
        }
        fprintf(stderr, "[surfinit] %08X loaded: mean B %llu G %llu R %llu A %llu, first %08X centre %08X\n", s->va,
                (unsigned long long)(sum[0] / (w * h)), (unsigned long long)(sum[1] / (w * h)),
                (unsigned long long)(sum[2] / (w * h)), (unsigned long long)(sum[3] / (w * h)),
                px[0], px[(h / 2) * w + w / 2]);
    }
    if (!make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &tmp, &tview, &tmem))
        return;
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.bufferRowLength = w;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = w;
    rg.imageExtent.height = h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    barrier_all();
    memset(&bl, 0, sizeof bl);
    bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
    bl.srcOffsets[1].x = (int32_t)w; bl.srcOffsets[1].y = (int32_t)h; bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = (int32_t)s->pw; bl.dstOffsets[1].y = (int32_t)s->ph; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(s_cb, tmp, VK_IMAGE_LAYOUT_GENERAL, s->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   VK_FILTER_LINEAR);
    barrier_all();
    garbage_add(tmp, tview, tmem);
}

static VkSurf *surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    VkSurf *s = surf_find(va), *victim = NULL;
    int i;

    /* h is the surface clip, not the allocation (NFSU2's split screen clips
     * the back buffer to each player's half): keep the surface while the
     * clip fits, grow it when it does not. Same as nv2a_gl.c. */
    if (s && s->w == w && s->h >= h) {
        s->used = s_frame;
        return s;
    }
    if (s && s->w == w && s->h > h)
        h = s->h;
    if (!s) {
        for (i = 0; i < VK_MAX_SURF; i++) {
            if (!s_surf[i].image) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->image) {
        if (s->va != va) {
            s_rt_evicts++;
            s_rt_gone[s_rt_gone_n++ & 255] = s->va;
        }
        if (s_rt == s) end_rendering(), s_rt = NULL;
        if (s_last == s) s_last = NULL;
        garbage_add(s->image, s->view, s->mem);
        if (s->view_a1) garbage_add(VK_NULL_HANDLE, s->view_a1, VK_NULL_HANDLE);
        if (s->ms_image) garbage_add(s->ms_image, s->ms_view, s->ms_mem);
    }
    memset(s, 0, sizeof *s);
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s_bind_epoch++;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    {
        double k = s_scale, m = (double)(w > h ? w : h);
        if (m * k > (double)s_max_size)
            k = (double)s_max_size / m;
        s->pw = (uint32_t)(w * k + 0.5); s->ph = (uint32_t)(h * k + 0.5);
        if (!s->pw) s->pw = 1;
        if (!s->ph) s->ph = 1;
    }
    if (!make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->view, &s->mem)) {
        LOGE("surface 0x%08X %ux%u: image creation failed\n", va, w, h);
        memset(s, 0, sizeof *s);
        return NULL;
    }
    {   /* The same image read with alpha 1 (see bind_stage) */
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &s->view_a1) != VK_SUCCESS)
            s->view_a1 = VK_NULL_HANDLE;
    }
    clear_image_color(s->image, 0, 0, 0, 1);
    if (s_msaa > VK_SAMPLE_COUNT_1_BIT && surf_msaa(w, h)) {
        s_img_samples = s_msaa;
        if (make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &s->ms_image, &s->ms_view, &s->ms_mem))
            clear_image_color(s->ms_image, 0, 0, 0, 1);
        else
            LOGE("surface 0x%08X: multisampled image failed\n", va);
        s_img_samples = VK_SAMPLE_COUNT_1_BIT;
        if (!s->ms_image) {         /* the pipelines assume s_msaa samples */
            garbage_add(s->image, s->view, s->mem);
            if (s->view_a1) garbage_add(VK_NULL_HANDLE, s->view_a1, VK_NULL_HANDLE);
            memset(s, 0, sizeof *s);
            return NULL;
        }
    }
    surf_init_from_memory(s, w, h);
    if (s_trace)
        LOGE("surface 0x%08X %ux%u (aa %ux%u, stored %ux%u)\n", va, w, h, aa_sx, aa_sy,
             s->pw, s->ph);
    return s;
}

static VkDepthBuf *depth_get(uint32_t va, uint32_t pw, uint32_t ph, VkSampleCountFlagBits samples)
{
    VkDepthBuf *d, *victim = NULL;
    int i;

    for (i = 0; i < VK_MAX_DEPTH; i++) {
        if (s_depth[i].image && s_depth[i].va == va && s_depth[i].pw == pw && s_depth[i].ph == ph
            && s_depth[i].samples == (uint32_t)samples) {
            s_depth[i].used = s_frame;
            return &s_depth[i];
        }
        if (!victim || (victim->image && (!s_depth[i].image || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->image) {
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
    }
    memset(d, 0, sizeof *d);
    d->va = va; d->pw = pw; d->ph = ph; d->used = s_frame;
    d->samples = (uint32_t)samples;
    s_img_samples = samples;
    if (!make_image(pw, ph, s_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    &d->image, &d->view, &d->mem)) {
        s_img_samples = VK_SAMPLE_COUNT_1_BIT;
        LOGE("depth 0x%08X %ux%u: image creation failed\n", va, pw, ph);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    s_img_samples = VK_SAMPLE_COUNT_1_BIT;
    {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        vkCmdClearDepthStencilImage(s_cb, d->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    if (s_trace)
        LOGE("depth 0x%08X %ux%u\n", va, pw, ph);
    return d;
}

/* The target of a draw or clear: surface (created on first use) and the
 * depth buffer of its zeta address. */
static VkSurf *target(const Nv2aSurface *sf, uint32_t zeta_va, VkDepthBuf **dout)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;   /* the clip need not start at 0 */
    VkSurf *s;

    *dout = NULL;
    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !sf->color_va || w > 4096 || h > 4096)
        return NULL;
    {
        uint64_t t0 = hz_now(), t1;
        s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1, sf->aa_sy ? sf->aa_sy : 1);
        t1 = hz_now();
        s_clr_ns[4] += t1 - t0;
        if (s && zeta_va)
            *dout = depth_get(zeta_va, s->pw, s->ph, s->ms_view ? s_msaa : VK_SAMPLE_COUNT_1_BIT);
        s_clr_ns[5] += hz_now() - t1;
    }
    if (s)
        s_last = s;
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkFormat fmt;
    uint32_t levels;
    uint32_t used, checked;
} VkTex;

#define VK_MAX_TEX 1024
#define TEX_BUCKETS 2048
static VkTex    s_tex[VK_MAX_TEX];
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[VK_MAX_TEX];
static uint64_t s_tex_bytes;
static uint32_t *s_decode;
static size_t    s_decode_cap;
static uint32_t s_palette_reg, s_palette_va;
static VkImage  s_dummy_img;
static VkImageView s_dummy_view;
static VkImage  s_dummy_cube_img;              /* for a cube stage with no texture */
static VkImageView s_dummy_cube_view;
static VkDeviceMemory s_dummy_cube_mem;
static VkDeviceMemory s_dummy_mem;

uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}
static void tex_index_add(VkTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}
static void tex_index_del(VkTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) { *at = s_tex_next[i]; return; }
        at = &s_tex_next[*at - 1];
    }
}

static int tex_size_from_format(uint32_t color)
{
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;
    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;
}

static uint32_t swizzle_index_slow(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;
    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* The swizzled offset is the OR of a part from u and a part from v, so per
 * size two tables make it two loads. Per texel the bit loop above was ~20
 * steps: a 512x512 texture Conker rewrites every frame of an in-game cutscene
 * took ~52 ms to decode on the console (16-20 fps there). Render thread only. */
static uint32_t s_swz_x[4096], s_swz_y[4096], s_swz_w, s_swz_h;

static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    if (w != s_swz_w || h != s_swz_h) {
        uint32_t i;
        if (w > 4096 || h > 4096)
            return swizzle_index_slow(u, v, w, h);
        for (i = 0; i < w; i++)
            s_swz_x[i] = swizzle_index_slow(i, 0, w, h);
        for (i = 0; i < h; i++)
            s_swz_y[i] = swizzle_index_slow(0, i, w, h);
        s_swz_w = w;
        s_swz_h = h;
    }
    return s_swz_x[u] | s_swz_y[v];
}

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;
    if (!(s_palette_reg & ~0x3Fu))
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t i = idx[swizzle_index(x, y, w, h)], c;
            if (i >= entries) i = 0;
            memcpy(&c, mem + s_palette_va + i * 4, 4);
            out[y * w + x] = c;
        }
    return 1;
}

static void tex_free(VkTex *t)
{
    s_bind_epoch++;
    garbage_add(t->image, t->view, t->mem);
    s_tex_bytes -= t->bytes;
    tex_index_del(t);
    memset(t, 0, sizeof *t);
}

static void tex_account(VkTex *t, uint32_t bytes)
{
    static uint64_t budget;
    s_hz_texs++;
    s_hz_tex_bytes += bytes;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        VkTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (c->image && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;
        tex_free(lru);
    }
}

/* Copy `bytes` at `src` (row length `row_texels`, 0 = tight) into the image. */
static void tex_upload_level(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels,
                             uint32_t layer, uint32_t level, uint32_t w, uint32_t h)
{
    void *dst;
    VkDeviceSize at = ring_alloc(bytes, 16, &dst);
    VkBufferImageCopy rg;
    s_uploads++;

    if (!dst)
        return;
    memcpy(dst, src, bytes);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.bufferRowLength = row_texels;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.mipLevel = level;
    rg.imageSubresource.baseArrayLayer = layer;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = w;
    rg.imageExtent.height = h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

static void tex_upload_layer(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels,
                             uint32_t layer)
{
    tex_upload_level(t, src, bytes, row_texels, layer, 0, t->w, t->h);
}

static void tex_upload(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels)
{
    tex_upload_layer(t, src, bytes, row_texels, 0);
}

/* Decode one 2D image (or cube face) at va into s_decode as B8G8R8A8.
 * Returns 0 when out of memory. */
static int tex_decode(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    Nv2aTexture nt;
    if ((size_t)w * h > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)w * h;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
    }
    if (!s_decode)
        return 0;
    if (color == 0x06 || color == 0x07) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + va);
        uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                s_decode[(size_t)y * w + x] = src[swizzle_index(x, y, w, h)] | fill;
    } else {
        int ok;
        memset(&nt, 0, sizeof nt);
        nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
        nt.addr_u = nt.addr_v = 1;
        ok = color == 0x0B ? decode_indexed(va, w, h, s_decode)
                           : nv2a_backend_decode_texture(&nt, s_decode);
        if (!ok) {
            uint32_t k;
            for (k = 0; k < w * h; k++)
                s_decode[k] = 0xFFFF00FFu;
            if (s_trace)
                LOGE("texture 0x%08X format 0x%02X not decodable\n", va, color);
        }
    }
    return 1;
}

/* RECOMP_VK_MIP_TINT=1 (diagnostic): mip level l >= 1 is a solid colour --
 * shows which level each pixel samples (red 1, green 2, blue 3, yellow 4,
 * magenta 5, cyan 6, white 7+). */
static int mip_tint_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VK_MIP_TINT"); on = e && *e == '1'; }
    return on;
}
static uint32_t mip_tint_argb(uint32_t l)
{
    static const uint32_t c[8] = { 0, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u,
                                   0xFFFF00FFu, 0xFF00FFFFu, 0xFFFFFFFFu };
    return c[l < 8 ? l : 7];
}
/* Solid BC1/BC2/BC3 blocks of an A8R8G8B8 colour into `out` (bytes). */
static void mip_tint_bc(uint8_t *out, uint32_t bytes, uint32_t color, uint32_t argb)
{
    uint16_t c565 = (uint16_t)((((argb >> 19) & 0x1F) << 11) | (((argb >> 10) & 0x3F) << 5) | ((argb >> 3) & 0x1F));
    uint32_t bs = color == 0x0C ? 8 : 16, i;
    for (i = 0; i + bs <= bytes; i += bs) {
        uint8_t *b = out + i, *cb = bs == 16 ? b + 8 : b;
        if (bs == 16) {
            memset(b, 0xFF, 8);                     /* BC2 alpha 0xF; BC3 a0 = a1 = 255 */
            if (color == 0x0F) memset(b + 2, 0, 6); /* BC3: index 0 = a0 */
        }
        cb[0] = (uint8_t)c565; cb[1] = (uint8_t)(c565 >> 8);
        cb[2] = (uint8_t)c565; cb[3] = (uint8_t)(c565 >> 8);
        memset(cb + 4, 0, 4);
    }
}

/* cube: a cube map (SET_TEXTURE_FORMAT bit 2) -- six faces, each with its
 * whole mip chain (levels), 128-byte aligned (nv2a_cube_face_bytes). Only
 * level 0 of each face is used. */
static VkImageView tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                           uint32_t levels, int cube)
{
    VkTex *t = NULL, *victim = NULL;
    uint32_t i, hash, face = 0, nfaces = 1, f;
    uint32_t key = color | (cube ? 0x100u : 0);    /* the cache tells the two apart */
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();

    if (cube) {
        face = nv2a_cube_face_bytes(color, w, h, levels);
        nfaces = 6;
        if (!face || w != h)
            return s_dummy_cube_view;
    }
    /* Mip levels as stored after level 0 (RECOMP_GL_MIPS=0: level 0 only,
     * as nv2a_gl.c). Swizzled/compressed only; a cube keeps level 0. */
    {
        static int mips = -1;
        if (mips < 0) {
            const char *e = getenv("RECOMP_GL_MIPS");
#if defined(__SWITCH__)
            /* Off by default on the console: NVK (GM20B) takes wrong derivatives
             * on triangle-edge pixels and samples small levels there -- dark lines
             * along edges (glass fixture: visible even with the level-2 cap,
             * gone without mips). RECOMP_GL_MIPS=1 turns them on. */
            mips = !(e && *e == '0');   /* on, with the level-2 cap (RECOMP_VK_MAX_LOD) as the baseline */
#else
            mips = !(e && *e == '0');
#endif
        }
        if (!mips || cube || pitch || !tex_size_from_format(color) || !nv2a_texel_bits(color))
            levels = 1;
        if (!levels) levels = 1;
        while (levels > 1 && ((w >> (levels - 1)) == 0 && (h >> (levels - 1)) == 0))
            levels--;
        /* Compressed chains stop at 4x4: NVK samples BC levels smaller than a
         * block wrongly -- dark dashes across mipmapped DXT surfaces on the
         * console, not on lavapipe (barn fixture; RECOMP_GL_MIPS=0 cleared
         * them). RECOMP_VK_BC_TAIL=1 keeps the full chain. */
        static int bc_tail = -1;
        if (bc_tail < 0) bc_tail = getenv("RECOMP_VK_BC_TAIL") != NULL;
        if (color >= 0x0C && color <= 0x0F && !bc_tail)
            while (levels > 1 && ((w >> (levels - 1)) < 4 || (h >> (levels - 1)) < 4))
                levels--;
        {   /* RECOMP_VK_MIN_MIP=<n> (diagnostic): no level smaller than n x n */
            static int mn = -1;
            if (mn < 0) { const char *e = getenv("RECOMP_VK_MIN_MIP"); mn = e ? atoi(e) : 0; }
            while (mn > 1 && levels > 1 &&
                   ((w >> (levels - 1)) < (uint32_t)mn || (h >> (levels - 1)) < (uint32_t)mn))
                levels--;
        }
    }
    for (i = s_tex_head[tex_bucket(va, key, w, h)]; i; i = s_tex_next[i - 1]) {
        VkTex *c = &s_tex[i - 1];
        if (c->image && c->va == va && c->color == key && c->w == w && c->h == h) {
            t = c;
            break;
        }
    }
    if (t && t->checked == s_frame) {
        t->used = s_frame;
        return t->view;
    }
    hash = bytes_hash(va, cube ? face * 6 : tex_bytes(color, w, h, pitch));
    if (color == 0x0B)
        hash ^= bytes_hash(s_palette_va, 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        return t->view;
    }
    if (s_have_bc && color == 0x0C) fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    if (s_have_bc && color == 0x0E) fmt = VK_FORMAT_BC2_UNORM_BLOCK;
    if (s_have_bc && color == 0x0F) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    if (t && (t->fmt != fmt || t->pitch != pitch || t->levels != levels)) {
        tex_free(t);
        t = NULL;
    }
    if (!t) {
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (!c->image) { if (!victim || victim->image) victim = c; }
            else if (!victim || (victim->image && c->used < victim->used)) victim = c;
        }
        t = victim;
        if (t->image)
            tex_free(t);
        t->va = va; t->color = key; t->w = w; t->h = h; t->pitch = pitch; t->fmt = fmt;
        t->levels = levels;
        VT("texture %08X fmt %02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
           cube ? " cube" : "");
        s_img_levels = levels;
        i = (uint32_t)make_image_ex(w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, cube, &t->image, &t->view, &t->mem);
        s_img_levels = 1;
        if (!i) {
            memset(t, 0, sizeof *t);
            return cube ? s_dummy_cube_view : s_dummy_view;
        }
        tex_index_add(t);
        s_bind_epoch++;
        if (s_trace)
            LOGE("texture 0x%08X format 0x%02X %ux%u pitch %u%s (face %u bytes)\n", va, color,
                 w, h, pitch, cube ? " cube" : "", face);
    }
    t->hash = hash;
    t->used = t->checked = s_frame;

    if (fmt != VK_FORMAT_B8G8R8A8_UNORM) {                 /* DXT as it is */
        uint32_t size = tex_bytes(color, w, h, 0), off = 0, l, total = 0;
        for (f = 0; f < nfaces; f++)
            tex_upload_layer(t, mem + va + f * face, size, 0, f);
        for (l = 0; l < levels; l++) {                      /* (cubes: levels 1) */
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            uint32_t ls = tex_bytes(color, lw, lh, 0);
            if (l)
            {
                if (mip_tint_on()) {
                    static uint8_t tint[1 << 20];
                    if (ls <= sizeof tint) {
                        mip_tint_bc(tint, ls, color, mip_tint_argb(l));
                        tex_upload_level(t, tint, ls, 0, 0, l, lw, lh);
                    }
                } else
                    tex_upload_level(t, mem + va + off, ls, 0, 0, l, lw, lh);
            }
            off += ls;
            total += ls;
        }
        tex_account(t, (cube ? size : total) * nfaces);
        return t->view;
    }
    if (!cube && color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        /* Linear A8R8G8B8 (movie frames): the bytes are B8G8R8A8 already. */
        tex_upload(t, mem + va, pitch * (h - 1) + w * 4, pitch / 4);
        tex_account(t, w * h * 4);
        return t->view;
    }
    for (f = 0; f < nfaces; f++) {
        if (!tex_decode(va + f * face, color, w, h, pitch))
            return cube ? s_dummy_cube_view : s_dummy_view;
        tex_upload_layer(t, s_decode, w * h * 4, 0, f);
    }
    {
        uint32_t l, off = w * h * nv2a_texel_bits(color) / 8, total = w * h * 4;
        for (l = 1; l < levels; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            if (!tex_decode(va + off, color, lw, lh, 0))
                break;
            if (mip_tint_on()) {
                uint32_t k2;
                for (k2 = 0; k2 < lw * lh; k2++)
                    s_decode[k2] = mip_tint_argb(l);
            }
            tex_upload_level(t, s_decode, lw * lh * 4, 0, 0, l, lw, lh);
            off += lw * lh * nv2a_texel_bits(color) / 8;
            total += lw * lh * 4;
        }
        tex_account(t, total * nfaces);
    }
    return t->view;
}

/* Samplers: wrap u, wrap v, mag, min. */
typedef struct { uint32_t key; VkSampler s; } SamplerEnt;
static SamplerEnt s_samplers[64];
static int s_nsamplers;

static VkSamplerAddressMode wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* SET_TEXTURE_BORDER_COLOR (A8R8G8B8) as the nearest of core Vulkan's three
 * fixed border colours: 0 transparent black, 1 opaque black, 2 opaque white.
 * Conker's projected blob shadow clamps to a white border (white = no
 * shadow); with Vulkan's default transparent black it showed as a dark box
 * around him (nv2a_gl.c sets the exact colour). */
static uint32_t border_class(uint32_t argb)
{
    uint32_t a = argb >> 24, l = ((argb >> 16) & 0xFF) + ((argb >> 8) & 0xFF) + (argb & 0xFF);
    if (l >= 3 * 128)
        return 2;               /* white: its alpha is lost (00FFFFFF: the shadow) */
    return a < 128 ? 0 : 1;
}

static VkSampler sampler_get(uint32_t key)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i;
    for (i = 0; i < s_nsamplers; i++)
        if (s_samplers[i].key == key)
            return s_samplers[i].s;
    ci.addressModeU = wrap_mode(key & 0xF);
    ci.addressModeV = wrap_mode((key >> 8) & 0xF);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.magFilter = (key & 0x10000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (key & 0x20000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = ((key >> 20) & 3) == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = ((key >> 20) & 3) ? VK_LOD_CLAMP_NONE : 0.0f;
    {   /* RECOMP_VK_MAX_LOD=<n>: no level past n. On NVK (GM20B) pixels along
         * triangle edges get derivatives that pick levels 5-6 (mip-tint
         * fixture run; lavapipe is fine with the same SPIR-V): dark dashes on
         * every mipmapped surface. A cap keeps nearby mips and makes the
         * wrong pick harmless. */
        static float cap = -2;
        if (cap == -2) {
            const char *e = getenv("RECOMP_VK_MAX_LOD");
#if defined(__SWITCH__)
            cap = e ? (float)atof(e) : 2.0f;    /* NVK: 676 -> 114 dark pixels, barn fixture */
#else
            cap = e ? (float)atof(e) : -1;
#endif
        }
        if (cap >= 0 && ((key >> 20) & 3))
            ci.maxLod = cap;
    }
    if (((key >> 20) & 3) && s_have_aniso) {
        /* RECOMP_GL_ANISO=<n> (default 4, as nv2a_gl.c) on mipmapped draws */
        static float an = -1;
        if (an < 0) { const char *e = getenv("RECOMP_GL_ANISO"); an = e ? (float)atof(e) : 4.0f; }
        if (an > 1.0f) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = an; }
    }
    /* bits 18-19: border colour class (bind_stage, border_class) */
    ci.borderColor = ((key >> 18) & 3) == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : ((key >> 18) & 3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                   : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (s_nsamplers == 64)
        s_nsamplers = 0;                    /* never in practice: wrap x filter x border keys */
    if (vkCreateSampler(s_dev, &ci, NULL, &s_samplers[s_nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    s_samplers[s_nsamplers].key = key;
    return s_samplers[s_nsamplers++].s;
}

/* A surface kept taller than its clip (surf_get) and sampled as a texture
 * of the clip's size: texcoords cover the w x h texels at the top left, not
 * the whole surface. A texture of the surface's logical or real size keeps
 * the old full-surface mapping (anti-aliased surfaces). */
static void rt_scale(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                     uint32_t aa_sx, uint32_t aa_sy, float scale[2])
{
    uint32_t lw = sw / (aa_sx ? aa_sx : 1), lh = sh / (aa_sy ? aa_sy : 1);

    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
}

/* Stage i: its image view (a surface or a cached texture), sampler and
 * texcoord scale. Records uploads, so call it outside rendering or accept
 * that it ends the pass. */
static void bind_stage(const uint32_t *regs, int i, float scale[2], VkDescriptorImageInfo *out,
                       VkSurf **sampled)
{
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, addr, filter, key;
    VkSurf *rt;
    int cache = 0;
    /* The shader declares a samplerCube for a CUBE_MAP stage (gl_psh.c), so
     * the view type follows the stage mode; the format's cube bit (2) says
     * the memory holds six faces. */
    int shader_cube = ((regs[0x1E70 / 4] >> (5 * i)) & 0x1F) == 3;
    int cube = shader_cube && (format & 4u);
    VkImageView none = shader_cube ? s_dummy_cube_view : s_dummy_view;

    /* The same stage state as this stage's last texture binding in this
     * frame, with no image made or freed since: the texture was already
     * checked this frame (tex_get), so the binding is the same. ~4.7k stage
     * binds a frame cost 3 ms in the barn fixture. Surfaces are not cached:
     * they may need a resolve. */
    static struct {
        uint32_t r[16], va, pal, mode, frame, epoch;
        float scale[2];
        VkDescriptorImageInfo out;
    } sc[4];
    uint32_t mode = (regs[0x1E70 / 4] >> (5 * i)) & 0x1F;
    if (sc[i].frame == s_frame && sc[i].epoch == s_bind_epoch && sc[i].va == s_batch->tex_va[i]
        && sc[i].pal == s_batch->pal_va[i] && sc[i].mode == mode
        && !memcmp(sc[i].r, regs + base, sizeof sc[i].r)) {
        scale[0] = sc[i].scale[0];
        scale[1] = sc[i].scale[1];
        *out = sc[i].out;
        *sampled = NULL;
        s_rep[11]++;
        return;
    }
    sc[i].frame = ~0u;

    scale[0] = scale[1] = 1.0f;
    out->imageView = none;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->sampler = sampler_get(0);
    *sampled = NULL;
    if (!(control0 & 0x40000000u) || !regs[base])
        return;
    va = s_batch->tex_va[i];
    if (tex_size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return;
    s_palette_reg = regs[base + 8];
    s_palette_va = s_batch->pal_va[i];
    {   /* RECOMP_GL_TEXLOG=1: each distinct (va, format, size) bound, once --
         * the same line as nv2a_gl.c, so the two back ends' logs diff. */
        static int tl = -1, n;
        static uint32_t seen[512];
        if (tl < 0) { const char *e = getenv("RECOMP_GL_TEXLOG"); tl = e && *e == '1'; }
        if (tl && n < 512) {
            uint32_t k0 = va ^ (color << 24) ^ (w << 12) ^ h, k;
            for (k = 0; k < (uint32_t)n && seen[k] != k0; k++) { }
            if (k == (uint32_t)n) {
                seen[n++] = k0;
                fprintf(stderr, "  [TEXBIND] stage %d va %08X fmt %02X %ux%u ctl0 %08X ctl1 %08X%s zeta %08X"
                        " texmodes %05X other %08X bump %08X %08X %08X %08X\n",
                        i, va, color, w, h, control0, regs[base + 4],
                        surf_find(va) ? " (colour surface)" : "", s_batch->zeta_va,
                        regs[0x1E70 / 4], regs[0x1E78 / 4], regs[base + 10], regs[base + 11],
                        regs[base + 12], regs[base + 13]);
            }
        }
    }
    rt = shader_cube ? NULL : surf_find(va);   /* a 2D surface can't be a cube */
    {   /* RECOMP_VK_NO_RT_TEX=<lo>-<hi> (diagnostic): sample guest memory, not
         * the render target, for textures in that range */
        static int on = -1;
        static uint32_t lo, hi;
        if (on < 0) {
            const char *e = getenv("RECOMP_VK_NO_RT_TEX");
            on = e && *e;
            if (on) { lo = (uint32_t)strtoul(e, NULL, 16); hi = strchr(e, '-') ? (uint32_t)strtoul(strchr(e, '-') + 1, NULL, 16) : lo; }
        }
        if (on && va >= lo && va <= hi)
            rt = NULL;
    }
    if (shader_cube && !cube) {
        static int warned;
        if (!warned++)
            LOGE("stage %d: cube-map mode on a non-cube texture 0x%08X (fmt %08X)\n",
                 i, va, format);
    } else if (rt) {
        /* An alpha-less format reads alpha 1 on the NV2A, but the image is
         * BGRA8 and holds whatever alpha the drawing shader wrote. Conker
         * samples its R5G6B5 light/shadow targets this way; with the stored
         * alpha its ground projections came out as teal patches (nv2a_gl.c). */
        int no_alpha = color == 0x05 || color == 0x03 || color == 0x07 ||
                       color == 0x11 || color == 0x1C || color == 0x1E;
        if (rt->ms_dirty && getenv("RECOMP_VK_RESLOG")) {
            static uint32_t nlog;
            if (nlog++ < 40)
                fprintf(stderr, "[reslog] frame %u: %08X %ux%u resolved for stage %d of a draw into %08X (%ux%u), fmt %02X, %u verts\n",
                        s_frame, rt->va, rt->pw, rt->ph, i, s_batch->surface.color_va, s_batch->surface.width,
                        s_batch->surface.height, color, s_batch->vertex_count);
        }
        if (rt->ms_dirty) s_post_va = rt->va;     /* RECOMP_VK_POSTLOG */
        surf_resolve(rt);
        out->imageView = no_alpha && rt->view_a1 ? rt->view_a1 : rt->view;
        *sampled = rt;
        rt_scale(w, h, rt->w, rt->h, rt->aa_sx, rt->aa_sy, scale);
    } else {
        uint32_t texs = s_hz_texs;
        uint64_t t0 = hz_now();
        {
            uint32_t k, n = s_rt_gone_n < 256 ? s_rt_gone_n : 256;
            for (k = 0; k < n; k++)
                if (s_rt_gone[k] == va) { s_rt_lost_binds++; break; }
        }
        out->imageView = tex_get(va, color, w, h, pitch, (format >> 16) & 0xF, cube);
        if (s_hz_texs != texs)
            s_hz_tex_ns += hz_now() - t0;
        cache = 1;
    }
    addr = regs[base + 2];
    filter = regs[base + 5];
    key = (addr & 0xF0F);
    if (((filter >> 24) & 0xF) == 1) key |= 0x10000;
    if (((filter >> 16) & 0xFF) == 1) key |= 0x20000;
    {   /* NV2A MIN 3/4: nearest mip; 5/6/7: between mips (nv2a_gl.c). A
         * render target has one level. */
        uint32_t mm = (filter >> 16) & 0xFF;
        if (!rt && mm >= 3 && mm <= 7)
            key |= (mm >= 5 ? 2u : 1u) << 20;
        if (mm == 3 || mm == 5) key |= 0x20000;     /* box: nearest within a level */
    }
    if ((addr & 0xF) == 4 || ((addr >> 8) & 0xF) == 4)
        key |= border_class(regs[base + 9]) << 18;
    out->sampler = sampler_get(key);
    if (cache) {
        memcpy(sc[i].r, regs + base, sizeof sc[i].r);
        sc[i].va = s_batch->tex_va[i];
        sc[i].pal = s_batch->pal_va[i];
        sc[i].mode = mode;
        sc[i].frame = s_frame;
        sc[i].epoch = s_bind_epoch;
        sc[i].scale[0] = scale[0];
        sc[i].scale[1] = scale[1];
        sc[i].out = *out;
    }
}

/* ── Shaders and pipelines ────────────────────────────────────────── */

/* std140 blocks (gl_vsh.c / gl_psh.c, Vulkan dialect). */
typedef struct {
    float c[192][4];
    float surf[4];
    float m[16];
    float vpoff[4];
    float aa[2];
    int32_t xform;
    int32_t pad;
} VsBlock;

typedef struct {
    float tscale[4][4];          /* vec2 array: 16-byte elements */
    float c0[8][4], c1[8][4];
    float fc0[4], fc1[4];
    float fog[4];
    int32_t afunc;
    float aref;
    float pad[2];
    float bump[4][4], bumpl[4][4];   /* BUMPENVMAP matrix / luminance (gl_psh.c) */
    float window_clip[8][4];
    int32_t window_control[4];     /* mode (-1 unrestricted, 0 include, 1 exclude), count */
} FsBlock;

/* NV097 window clipping is the union of eight inclusive 12-bit rectangles,
 * independent of SET_SURFACE_CLIP. Keep it in fragment state so disjoint
 * regions and exclusive holes never draw a fragment twice. */
static void window_clip_block(FsBlock *fb, const uint32_t *r, const VkSurf *s)
{
    uint32_t mode = r[0x2B4 / 4] & 1u;
    uint32_t count = 0;
    fb->window_control[0] = (int32_t)mode;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t h = r[0x2C0 / 4 + i], v = r[0x2E0 / 4 + i];
        uint32_t x0 = (h & 0xFFFu) * s->aa_sx;
        uint32_t y0 = (v & 0xFFFu) * s->aa_sy;
        uint32_t x1 = (((h >> 16) & 0xFFFu) + 1u) * s->aa_sx;
        uint32_t y1 = (((v >> 16) & 0xFFFu) + 1u) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 <= x0 || y1 <= y0) continue;
        if (!mode && !x0 && !y0 && x1 == s->w && y1 == s->h) {
            /* The common full-window case needs no per-fragment test. */
            memset(fb->window_clip, 0, sizeof fb->window_clip);
            fb->window_control[0] = -1;
            fb->window_control[1] = 0;
            return;
        }
        float box[4] = { (float)to_stored(x0, s->pw, s->w), (float)to_stored(y0, s->ph, s->h),
                         (float)to_stored(x1, s->pw, s->w), (float)to_stored(y1, s->ph, s->h) };
        uint32_t j;
        for (j = 0; j < count; j++)
            if (!memcmp(box, fb->window_clip[j], sizeof box)) break;
        if (j == count) memcpy(fb->window_clip[count++], box, sizeof box);
    }
    fb->window_control[1] = (int32_t)count;
    if (mode && !count) fb->window_control[0] = -1;
}

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_layout;
/* RECOMP_VK_DYNUBO (default 1): the two uniform blocks as dynamic UBOs in
 * set 1 (one set per frame slot, on its ring; the per-draw offsets go with
 * vkCmdBindDescriptorSets) and only the textures pushed. NVK binds dynamic
 * UBOs as hardware constant buffers; pushed UBOs were read like memory: on
 * the console the transform constants cost about as much as all the
 * vertex-program arithmetic (PERF.md, 2026-10-09). */
static int                   s_dynubo = -1;
static VkDescriptorSetLayout s_dsl_ubo;
static VkDescriptorPool      s_ubo_pool;
static VkDescriptorSet       s_ubo_set[2];
static VkPipelineCache       s_pcache;
static int s_pcache_loaded;

typedef struct {
    uint64_t   vkey;
    Nv2aPshKey pkey;
    uint32_t   cmp;             /* attributes bound as packed CMP words (s_cmp) */
    VkShaderModule vs, fs;
    uint32_t   used;
    uint32_t   hash;
    int        next;
} VkProg;

#define VK_MAX_PROG 1024
#define PROG_BUCKETS 1024
static VkProg s_prog[VK_MAX_PROG];
static int s_prog_head[PROG_BUCKETS];
static uint32_t s_prog_made, s_prog_live;

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }
/* The GL renderer's thread statistics; the Vulkan one records on the
 * executor's thread. */
void nv2a_gl_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    (void)secs;
    if (gl_idle_pct) *gl_idle_pct = 0;
    if (exec_wait_pct) *exec_wait_pct = 0;
}

typedef struct {
    glslang_stage_t stage;
    const char *src;
    uint32_t *words;
    size_t nwords;
    int ok;
} CompileJob;

static void *compile_thread(void *arg)
{
    CompileJob *j = (CompileJob *)arg;
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = j->stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = j->src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        LOGE("%s shader failed:\n%s\n", j->stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
             glslang_shader_get_info_log(sh));
        if (s_trace)
            fprintf(stderr, "%s\n", j->src);
        glslang_shader_delete(sh);
        return NULL;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        LOGE("link failed: %s\n", glslang_program_get_info_log(pr));
    } else {
        glslang_program_SPIRV_generate(pr, j->stage);
        j->nwords = glslang_program_SPIRV_get_size(pr);
        j->words = (uint32_t *)malloc(j->nwords * 4);
        if (j->words) {
            glslang_program_SPIRV_get(pr, j->words);
            j->ok = 1;
        }
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return NULL;
}

/* ── Caches across runs ───────────────────────────────────────────
 *
 * A program compiled at first use stalled a race start for ~1 s (glslang,
 * then NIR and NAK inside NVK: ~50 ms each on the console). Three files, next
 * to the NRO (the current directory on Linux); RECOMP_PROG_CACHE=0 disables
 * them all, =<path> moves the program list:
 *   progcache.bin    what each program was built from -- the GL renderer's
 *                    records, shared (the format does not depend on the back end)
 *   vkspirv.bin      glslang's output by hash of the GLSL source
 *   vkpipes.bin      the pipeline variants seen (program + fixed state)
 *   vkpipecache.bin  the driver's VkPipelineCache (NAK binaries), saved while
 *                    running whenever pipelines were added
 * At start-up every recorded program and pipeline is built before the title
 * draws. */
static int s_prewarming;
static int s_prewarm_early;      /* prewarm before the title runs: a longer budget */
static int s_cache_off = -1;

extern const char *xbox_platform_shader_cache_dir(void) __attribute__((weak));

static const char *cache_dir(void)
{
#if defined(__SWITCH__)
    if (xbox_platform_shader_cache_dir) {
        const char *p = xbox_platform_shader_cache_dir();
        if (p && *p) return p;
    }
    return "sdmc:/switch/conkerx/";
#else
    return "";
#endif
}

static int cache_path(char *out, size_t cap, const char *name)
{
    if (s_cache_off < 0) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        s_cache_off = e && e[0] == '0' && !e[1];
    }
    if (s_cache_off)
        return 0;
    if (!strcmp(name, "progcache.bin")) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        if (e && *e) {
            snprintf(out, cap, "%s", e);
            return 1;
        }
    }
    snprintf(out, cap, "%s%s", cache_dir(), name);
    return 1;
}

#define SPV_MAGIC 0x31565053u                   /* "SPV1" */
typedef struct { uint64_t hash; uint32_t stage, nwords; uint32_t *words; } SpvEnt;
static SpvEnt *s_spv;
static uint32_t s_nspv, s_spv_cap, s_spv_hits;

static uint64_t text_hash(const char *t, uint32_t stage)
{
    uint64_t h = 1469598103934665603ull ^ stage;
    while (*t)
        h = (h ^ (uint8_t)*t++) * 1099511628211ull;
    return h;
}

static void spv_add(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    if (s_nspv == s_spv_cap) {
        s_spv_cap = s_spv_cap ? s_spv_cap * 2 : 256;
        s_spv = (SpvEnt *)realloc(s_spv, s_spv_cap * sizeof *s_spv);
    }
    s_spv[s_nspv].hash = hash;
    s_spv[s_nspv].stage = stage;
    s_spv[s_nspv].nwords = n;
    s_spv[s_nspv].words = (uint32_t *)malloc((size_t)n * 4);
    memcpy(s_spv[s_nspv].words, w, (size_t)n * 4);
    s_nspv++;
}

static const SpvEnt *spv_find(uint64_t hash, uint32_t stage)
{
    uint32_t i;
    for (i = 0; i < s_nspv; i++)
        if (s_spv[i].hash == hash && s_spv[i].stage == stage)
            return &s_spv[i];
    return NULL;
}

/* The caches only ever grow: every generator change appends new SPIR-V
 * and program records and nothing drops the old ones (235 MB of SPIR-V and
 * 2.5k program records on the console after a few days of builds: a 3.6 s
 * load and a prewarm spending its 2 s budget on stale entries). Over a cap
 * at start-up, all four start again; they refill in one session. */
static void cache_cap(void)
{
    static const char *const names[4] = { "vkspirv.bin", "progcache.bin", "vkpipes.bin", "vkpipecache.bin" };
    static const long caps[2] = { 32L << 20, 2L << 20 };
    char path[256];
    int i, over = 0;
    for (i = 0; i < 2; i++) {
        FILE *f;
        if (!cache_path(path, sizeof path, names[i]) || !(f = fopen(path, "rb")))
            continue;
        if (fseek(f, 0, SEEK_END) == 0 && ftell(f) > caps[i])
            over = 1;
        fclose(f);
    }
    if (!over)
        return;
    for (i = 0; i < 4; i++)
        if (cache_path(path, sizeof path, names[i]))
            remove(path);
    fprintf(stderr, "  [VK] shader/pipeline caches over their caps: cleared\n");
}

static void spv_load(void)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    cache_cap();
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "rb")))
        return;
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        uint64_t hash;
        uint32_t *w;
        if (hdr[0] != SPV_MAGIC || hdr[3] > (1u << 22))
            break;
        memcpy(&hash, &hdr[1], 8);
        w = (uint32_t *)malloc((size_t)hdr[3] * 8 + 8);
        if (!w || fread(w, 4, hdr[3], f) != hdr[3]) { free(w); break; }
        /* the stage is in the word after the header */
        spv_add(hash, w[0], w + 1, hdr[3] - 1);
        free(w);
    }
    fclose(f);
}

/* Cache appends go to memory and reach the files in one write each, at most
 * every few seconds (cache_flush from vk_flip). Appending straight to the SD
 * card -- open, write, close per program and per pipeline -- was ~10% of the
 * push-buffer thread on the console (fsdev_open/write/close in compile and
 * pipe_get), and that thread is the one the title waits on. */
typedef struct { const char *name; uint8_t *d; size_t n, cap; } CacheAppend;
static CacheAppend s_app[3] = { { "vkspirv.bin" }, { "progcache.bin" }, { "vkpipes.bin" } };

static void cache_append(int which, const void *src, size_t n)
{
    CacheAppend *a = &s_app[which];
    if (a->n + n > a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 65536;
        uint8_t *d;
        while (cap < a->n + n) cap *= 2;
        if (!(d = (uint8_t *)realloc(a->d, cap)))
            return;
        a->d = d;
        a->cap = cap;
    }
    memcpy(a->d + a->n, src, n);
    a->n += n;
}

static void cache_flush_sync(void)
{
    char path[256];
    int i;
    for (i = 0; i < 3; i++) {
        CacheAppend *a = &s_app[i];
        FILE *f;
        if (!a->n || !cache_path(path, sizeof path, a->name))
            continue;
        if ((f = fopen(path, "ab"))) {
            fwrite(a->d, 1, a->n, f);
            fclose(f);
        }
        a->n = 0;
    }
}

static void spv_append(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    char path[256];
    uint32_t hdr[5];
    if (!cache_path(path, sizeof path, "vkspirv.bin"))
        return;
    hdr[0] = SPV_MAGIC;
    memcpy(&hdr[1], &hash, 8);
    hdr[3] = n + 1;
    hdr[4] = stage;
    cache_append(0, hdr, sizeof hdr);
    cache_append(0, w, (size_t)n * 4);
}

/* GLSL to a shader module. glslang's parser recurses deeply: it runs on a
 * thread with a 4 MB stack (the executor's is smaller). */
static VkShaderModule compile(glslang_stage_t stage, const char *src)
{
    CompileJob j = { stage, src, NULL, 0, 0 };
    pthread_t t;
    pthread_attr_t at;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    uint64_t hash = text_hash(src, (uint32_t)stage);
    const SpvEnt *hit = spv_find(hash, (uint32_t)stage);

    if (hit) {
        s_spv_hits++;
        ci.codeSize = (size_t)hit->nwords * 4;
        ci.pCode = hit->words;
        vkCreateShaderModule(s_dev, &ci, NULL, &m);
        return m;
    }
    VT("compile %s (%u bytes of GLSL)\n", stage == GLSLANG_STAGE_VERTEX ? "vs" : "fs",
       (unsigned)strlen(src));
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, compile_thread, &j) == 0)
        pthread_join(t, NULL);
    else
        compile_thread(&j);
    pthread_attr_destroy(&at);
    if (!j.ok)
        return VK_NULL_HANDLE;
    ci.codeSize = j.nwords * 4;
    ci.pCode = j.words;
    VT("  SPIR-V %u words -> vkCreateShaderModule\n", (unsigned)j.nwords);
    vkCreateShaderModule(s_dev, &ci, NULL, &m);
    spv_add(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    spv_append(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    free(j.words);
    return m;
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;
    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)
            break;
    }
    return h ? h : 1;
}

static uint32_t prog_bucket(uint64_t vkey, const Nv2aPshKey *pk)
{
    const uint32_t *w = (const uint32_t *)pk;
    uint64_t h = vkey ^ 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *pk / 4; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (PROG_BUCKETS - 1);
}

static void pipes_forget_prog(int prog);

/* progcache.bin records: nv2a_gl.c's ProgRec, byte for byte (xform bits
 * 16-31: s_cmp, Vulkan's own cache). */
static uint32_t s_cmp;
#define PROG_REC_MAGIC 0x4350564Eu              /* "NVPC" */
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;

static void prog_rec_fill(ProgRec *rec, const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    memset(rec, 0, sizeof *rec);
    rec->magic = PROG_REC_MAGIC;
    rec->xform = b->xform == 2 ? 2 : 1;
    rec->pk = *pk;
    if (rec->xform == 2 && b->vp_program && b->vp_slots <= 136) {
        rec->vp_start = b->vp_start;
        rec->slots = b->vp_slots;
        memcpy(rec->prog, b->vp_program, (size_t)b->vp_slots * 16);
    }
    rec->xform |= s_cmp << 16;          /* the CMP attribute mask (Vulkan only) */
}

static void prog_rec_append(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    char path[256];
    ProgRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "progcache.bin"))
        return;
    prog_rec_fill(&rec, b, pk);
    (void)f;
    cache_append(1, &rec, sizeof rec);
}

/* The batch a record describes (vertex program slots in rec). */
static void prog_rec_batch(const ProgRec *rec, Nv2aRawBatch *bb, uint32_t n)
{
    memset(bb, 0, sizeof *bb);
    bb->xform = rec->xform & 0xFFFF;
    s_cmp = rec->xform >> 16;
    bb->vp_program = (const uint32_t (*)[4])rec->prog;
    bb->vp_slots = rec->slots ? rec->slots : 136;
    bb->vp_start = rec->vp_start;
    bb->vp_prog_gen = 0x80000000u + n;          /* hash it: not an executor version */
}

/* s_cmp: CMP attributes (NV2A_BACKEND_RAW_CMP) of the draw being recorded:
 * bound as R32_SINT, so the program declares them int and unpacks them as
 * the executor did (x, y: signed 11 bits / 1023, z: signed 10 bits / 511, w 1). */

static int cmp_patch(char *src, size_t cap, uint32_t mask)
{
    static const char fn[] =
        "vec4 nv2a_cmp(int p) {\n"
        "    return vec4(float(bitfieldExtract(p, 0, 11)) / 1023.0, float(bitfieldExtract(p, 11, 11)) / 1023.0,\n"
        "                float(bitfieldExtract(p, 22, 10)) / 511.0, 1.0);\n"
        "}\n";
    char from[64], to[96], init[512], *q;
    size_t n = strlen(src), m = 0;
    uint32_t a;
    init[0] = 0;
    for (a = 0; a < 16; a++) {
        if (!(mask & (1u << a)))
            continue;
        snprintf(from, sizeof from, "layout(location = %u) in vec4 v%u;\n", a, a);
        snprintf(to, sizeof to, "layout(location = %u) in int v%up;\nvec4 v%u;\n", a, a, a);
        if (!(q = strstr(src, from)) || n + strlen(to) - strlen(from) >= cap)
            return 0;
        memmove(q + strlen(to), q + strlen(from), n - (size_t)(q - src) - strlen(from) + 1);
        memcpy(q, to, strlen(to));
        n += strlen(to) - strlen(from);
        m += (size_t)snprintf(init + m, sizeof init - m, "    v%u = nv2a_cmp(v%up);\n", a, a);
    }
    if (!(q = strstr(src, "void main() {\n")) || n + sizeof fn + m >= cap)
        return 0;
    /* the helper before main, the unpacking first thing in it */
    memmove(q + (sizeof fn - 1) + 14 + m, q + 14, n - (size_t)(q - src) - 14 + 1);
    memmove(q + (sizeof fn - 1), q, 14);
    memcpy(q, fn, sizeof fn - 1);
    memcpy(q + (sizeof fn - 1) + 14, init, m);
    return 1;
}

static VkProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    static uint32_t last_gen;
    static uint64_t last_vkey;
    static VkProg *last;
    static char vsrc[320 * 1024], fsrc[64 * 1024];
    uint64_t vkey = 0;
    uint32_t bucket;
    VkProg *p, *victim = NULL;
    int i;
    size_t n;

    if (b->xform == 2) {
        if (b->vp_prog_gen != last_gen || !last_vkey) {
            last_vkey = prog_hash(b);
            last_gen = b->vp_prog_gen;
        }
        vkey = last_vkey;
    }
    if (last && last->vs && last->vkey == vkey && last->cmp == s_cmp && !memcmp(&last->pkey, pk, sizeof *pk)) {
        last->used = s_frame;
        return last;
    }
    bucket = prog_bucket(vkey, pk);
    for (i = s_prog_head[bucket]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && c->cmp == s_cmp && !memcmp(&c->pkey, pk, sizeof *pk)) {
            c->used = s_frame;
            last = c;
            return c;
        }
    }
    for (i = 0; i < VK_MAX_PROG; i++) {
        VkProg *c = &s_prog[i];
        if (!c->vs) { if (!victim || victim->vs) victim = c; }
        else if (!victim || (victim->vs && c->used < victim->used)) victim = c;
    }
    p = victim;
    last = NULL;
    if (p->vs) {
        int *at = &s_prog_head[p->hash], me = (int)(p - s_prog) + 1;
        while (*at) {
            if (*at == me) { *at = p->next; break; }
            at = &s_prog[*at - 1].next;
        }
        pipes_forget_prog((int)(p - s_prog));
        vkDestroyShaderModule(s_dev, p->vs, NULL);
        vkDestroyShaderModule(s_dev, p->fs, NULL);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);

    n = (size_t)snprintf(vsrc, sizeof vsrc, "%s", nv2a_gl_vsh_prelude());
    if (vkey) {
        int k = nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, vsrc + n, sizeof vsrc - n);
        if (k < 0)
            return NULL;
        n += (size_t)k;
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_main_program());
    } else {
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_fixed());
    }
    if (nv2a_gl_psh(pk, fsrc, sizeof fsrc) < 0)
        return NULL;
    if (s_cmp && !cmp_patch(vsrc, sizeof vsrc, s_cmp))
        return NULL;
    {   /* RECOMP_SHADER_DUMP=<dir>: this program in both dialects, as
         * <dir>/<n>_<modes>_{vk,gl}.{vert,frag} -- the same generator calls
         * nv2a_gl.c makes, so the pair differs only where the dialects do
         * (diagnostic). */
        static const char *dir;
        static int init, seq;
        if (!init) { init = 1; dir = getenv("RECOMP_SHADER_DUMP"); }
        if (dir && *dir && !s_prewarming) {
            static char gv[320 * 1024], gf[64 * 1024];
            char path[512];
            FILE *f;
            size_t m;
            int d;
            nv2a_shader_vk = 0;
            m = (size_t)snprintf(gv, sizeof gv, "%s", nv2a_gl_vsh_prelude());
            if (vkey) {
                int k = nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, gv + m, sizeof gv - m);
                if (k > 0) m += (size_t)k;
                snprintf(gv + m, sizeof gv - m, "%s", nv2a_gl_vsh_main_program());
            } else {
                snprintf(gv + m, sizeof gv - m, "%s", nv2a_gl_vsh_fixed());
            }
            if (nv2a_gl_psh(pk, gf, sizeof gf) < 0)
                gf[0] = 0;
            nv2a_shader_vk = 1;
            for (d = 0; d < 4; d++) {
                const char *src = d == 0 ? vsrc : d == 1 ? fsrc : d == 2 ? gv : gf;
                snprintf(path, sizeof path, "%s/%04d_%05X_%s.%s", dir, seq, pk->shader_program,
                         d < 2 ? "vk" : "gl", (d & 1) ? "frag" : "vert");
                if ((f = fopen(path, "w"))) { fputs(src, f); fclose(f); }
            }
            seq++;
        }
    }
    if (getenv("RECOMP_PSH_NULL") && *getenv("RECOMP_PSH_NULL") == '1')   /* cost experiment */
        snprintf(fsrc, sizeof fsrc, "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.5); }\n");
    if (s_dynubo) {     /* the uniform blocks live in set 1 */
        char *q;
        if ((q = strstr(vsrc, "set = 0, binding = 0) uniform VsU"))) q[6] = '1';
        if ((q = strstr(fsrc, "set = 0, binding = 1) uniform FsU"))) q[6] = '1';
    }
    p->vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    p->fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (!p->vs || !p->fs) {
        if (p->vs) vkDestroyShaderModule(s_dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(s_dev, p->fs, NULL);
        memset(p, 0, sizeof *p);
        return NULL;
    }
    s_prog_live++;
    if ((++s_prog_made % 100) == 0)
        LOGE("%u shader programs compiled (%u live)\n", s_prog_made, s_prog_live);
    p->vkey = vkey;
    p->cmp = s_cmp;
    p->pkey = *pk;
    p->used = s_frame;
    p->hash = bucket;
    p->next = s_prog_head[bucket];
    s_prog_head[bucket] = (int)(p - s_prog) + 1;
    last = p;
    prog_rec_append(b, pk);
    return p;
}

typedef struct {
    int      prog;
    uint32_t blend, bsrc, bdst, beq, cmask, depth, topo;
    uint32_t samples;           /* the target's: s_msaa or 1 (surf_msaa) */
} PipeKey;

typedef struct {
    PipeKey key;
    VkPipeline pipe;
    uint32_t hash;
    int next;
} PipeEnt;

#define VK_MAX_PIPE 4096
#define PIPE_BUCKETS 4096
static PipeEnt s_pipes[VK_MAX_PIPE];
static int s_pipe_head[PIPE_BUCKETS];
static int s_npipes;

static void pipes_forget_prog(int prog)
{
    int i;
    /* Rare (1024 live programs): drop every pipeline using it. */
    for (i = 0; i < s_npipes; i++) {
        PipeEnt *e = &s_pipes[i];
        if (e->pipe && e->key.prog == prog) {
            int *at = &s_pipe_head[e->hash], me = i + 1;
            while (*at) {
                if (*at == me) { *at = e->next; break; }
                at = &s_pipes[*at - 1].next;
            }
            garbage_add_pipe(e->pipe);               /* may still be in flight */
            e->pipe = VK_NULL_HANDLE;
        }
    }
}

static VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

/* vkpipes.bin: the program by what generated it, and the pipeline's own
 * state. */
#define PIPE_REC_MAGIC 0x4C504B56u              /* "VKPL" */
typedef struct {
    uint32_t   magic, pad;
    uint64_t   vkey;
    Nv2aPshKey pk;
    uint32_t   blend, bsrc, bdst, beq, cmask, depth, topo;
} PipeRec;
static uint32_t s_pipes_unsaved;

static void pipe_rec_append(const PipeKey *k)
{
    char path[256];
    PipeRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "vkpipes.bin"))
        return;
    memset(&rec, 0, sizeof rec);
    rec.magic = PIPE_REC_MAGIC;
    rec.vkey = s_prog[k->prog].vkey;
    rec.pk = s_prog[k->prog].pkey;
    rec.blend = k->blend; rec.bsrc = k->bsrc; rec.bdst = k->bdst; rec.beq = k->beq;
    rec.cmask = k->cmask; rec.depth = k->depth; rec.topo = k->topo;
    rec.pad = k->samples;       /* 0 in records older than per-surface MSAA */
    (void)f;
    cache_append(2, &rec, sizeof rec);
}

static VkPipeline pipe_get(const PipeKey *k)
{
    const uint32_t *w = (const uint32_t *)k;
    uint32_t h = 2166136261u, i;
    int e;
    PipeEnt *pe;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkProg *p = &s_prog[k->prog];

    for (i = 0; i < sizeof *k / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    h &= PIPE_BUCKETS - 1;
    for (e = s_pipe_head[h]; e; e = s_pipes[e - 1].next)
        if (s_pipes[e - 1].pipe && !memcmp(&s_pipes[e - 1].key, k, sizeof *k))
            return s_pipes[e - 1].pipe;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = p->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = p->fs;
    st[1].pName = "main";
    ia.topology = k->topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;       /* values per draw: apply_dynamic, 0 when off */
    rs.depthClampEnable = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    ms.rasterizationSamples = k->samples ? (VkSampleCountFlagBits)k->samples : s_msaa;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(k->bsrc);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(k->bdst);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(k->beq);
    ba.colorWriteMask = k->cmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (k->depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_layout;
    VT("vkCreateGraphicsPipelines prog %d blend %u cmask %X depth %u topo %u\n", k->prog, k->blend,
       k->cmask, k->depth, k->topo);
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOGE("pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    if (s_npipes == VK_MAX_PIPE) {
        LOGE("pipeline table full\n");
        return pipe;                          /* leaks; never seen in practice */
    }
    pe = &s_pipes[s_npipes];
    pe->key = *k;
    pe->pipe = pipe;
    pe->hash = h;
    pe->next = s_pipe_head[h];
    s_pipe_head[h] = ++s_npipes;
    /* Recreating an already recorded pipeline during prewarm does not add
     * a cache key. Avoid rewriting the entire driver blob at every boot. */
    if (!s_prewarming || !s_pcache_loaded) s_pipes_unsaved++;
    pipe_rec_append(k);
    return pipe;
}

/* RECOMP_VK_SHADER_RESOLVE (default 1): resolve a multisampled surface with
 * a full-screen pass that averages the samples (texelFetch), not
 * vkCmdResolveImage -- NVK's took ~1.7 ms per 960x720 4x resolve on the
 * console, several times the bandwidth it needs.
 *
 * RECOMP_VK_PRESENT_MS=1 (default off: no throughput gain on the console): the present pass reads the multisampled
 * image straight into the swapchain image (sample average + bilinear scale),
 * instead of a resolve into the surface and a blit -- one full-surface write
 * and read fewer per frame. */
static int                   s_sres = -1;
static VkDescriptorSetLayout s_sres_dsl;
static VkPipelineLayout      s_sres_layout;
static VkPipeline            s_sres_pipe;
static VkSampler             s_sres_sampler;
extern float xbox_platform_visibility(void) __attribute__((weak));
extern const char *xbox_platform_pause_option(void) __attribute__((weak));
static int                   s_pres = -1;
static VkPipelineLayout      s_pres_layout;
static VkPipeline            s_pres_pipe, s_pres_ss_pipe;
static VkFormat              s_pres_fmt, s_pres_ss_fmt;

/* A full-screen-triangle pipeline: one colour attachment, no blending. */
static VkPipeline fs_pipeline(const char *fs, VkPipelineLayout layout, VkFormat cfmt,
                              VkSampleCountFlagBits samples)
{
    VkShaderModule vm, fm;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    static const char vs[] =
        "#version 450\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";

    vm = compile(GLSLANG_STAGE_VERTEX, vs);
    fm = compile(GLSLANG_STAGE_FRAGMENT, fs);
    if (!vm || !fm) {
        LOGE("full-screen pass: compile failed\n");
        return VK_NULL_HANDLE;
    }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vm;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fm;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = samples;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout;
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOGE("full-screen pass: pipeline creation failed\n");
        pipe = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(s_dev, vm, NULL);
    vkDestroyShaderModule(s_dev, fm, NULL);
    return pipe;
}

static int sres_init(void)
{
    char fs[1024];
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i, n = (int)s_msaa;

    if (s_sres >= 0)
        return s_sres;
    {
        const char *e = getenv("RECOMP_VK_SHADER_RESOLVE");
        s_sres = !(e && *e == '0');
    }
    if (!s_sres)
        return 0;
    s_sres = 0;
    {
        size_t m = (size_t)snprintf(fs, sizeof fs,
            "#version 450\n"
            "layout(set = 0, binding = 0) uniform sampler2DMS t;\n"
            "layout(location = 0) out vec4 o;\n"
            "void main() {\n"
            "    ivec2 c = ivec2(gl_FragCoord.xy);\n"
            "    vec4 a = texelFetch(t, c, 0);\n");
        for (i = 1; i < n; i++)
            m += (size_t)snprintf(fs + m, sizeof fs - m, "    a += texelFetch(t, c, %d);\n", i);
        snprintf(fs + m, sizeof fs - m, "    o = a * %.6f;\n}\n", 1.0 / n);
    }
    memset(&b, 0, sizeof b);
    b.binding = 0;
    b.descriptorCount = 1;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_sres_dsl) != VK_SUCCESS)
        return 0;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s_sres_dsl;
    if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_sres_layout) != VK_SUCCESS)
        return 0;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(s_dev, &sci, NULL, &s_sres_sampler) != VK_SUCCESS)
        return 0;
    if (!(s_sres_pipe = fs_pipeline(fs, s_sres_layout, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT)))
        return 0;
    s_sres = 1;
    fprintf(stderr, "  [VK] shader resolve on (%d samples)\n", n);
    return 1;
}

/* The present pass's pipeline for the current swapchain format. Push
 * constants: destination origin, source texels per destination pixel,
 * source size - 1. Bilinear over per-texel sample averages, exactly the
 * old resolve + linear blit; a 1:1 copy takes one tap. */
static int pres_init(int multisampled)
{
    char fs[3072];
    VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int i, n = multisampled ? (int)s_msaa : 1;
    VkPipeline *pipe = multisampled ? &s_pres_pipe : &s_pres_ss_pipe;
    VkFormat *format = multisampled ? &s_pres_fmt : &s_pres_ss_fmt;
    size_t m;

    if (s_pres == 0)
        return 0;
    if (s_pres < 0) {
        const char *e = getenv("RECOMP_VK_PRESENT_MS");
        if ((!(e && *e == '1') && !xbox_platform_visibility) || !sres_init()) {
            s_pres = 0;
            return 0;
        }
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_sres_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_pres_layout) != VK_SUCCESS) {
            s_pres = 0;
            return 0;
        }
        s_pres = 1;
    }
    if (*pipe && *format == s_sc_format)
        return 1;
    if (*pipe)
        vkDestroyPipeline(s_dev, *pipe, NULL);
    m = (size_t)snprintf(fs, sizeof fs,
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform %s t;\n"
        "layout(push_constant) uniform P { vec2 off; vec2 scl; ivec2 smax; float visibility; } p;\n"
        "layout(location = 0) out vec4 o;\n"
        "vec4 tap(ivec2 c) {\n"
        "    c = clamp(c, ivec2(0), p.smax);\n"
        "    vec4 a = texelFetch(t, c, 0);\n", multisampled ? "sampler2DMS" : "sampler2D");
    for (i = 1; i < n; i++)
        m += (size_t)snprintf(fs + m, sizeof fs - m, "    a += texelFetch(t, c, %d);\n", i);
    snprintf(fs + m, sizeof fs - m,
        "    return a * %.6f;\n"
        "}\n"
        "void main() {\n"
        "    vec2 s = (gl_FragCoord.xy - p.off) * p.scl - 0.5;\n"
        "    vec2 f = fract(s);\n"
        "    ivec2 i = ivec2(floor(s));\n"
        "    if (f == vec2(0.0)) o = tap(i); else\n"
        "    o = mix(mix(tap(i), tap(i + ivec2(1, 0)), f.x),\n"
        "            mix(tap(i + ivec2(0, 1)), tap(i + ivec2(1, 1)), f.x), f.y);\n"
        "    if (p.visibility > 0.0) {\n"
        "        float l = dot(o.rgb, vec3(0.299, 0.587, 0.114));\n"
        "        float d = 1.0 - l;\n"
        "        float lift = p.visibility * smoothstep(0.012, 0.06, l) * d*d*d;\n"
        "        o.rgb = clamp(o.rgb * (1.0 + lift / max(l, 0.012)), 0.0, 1.0);\n"
        "    }\n"
        "}\n", 1.0 / n);
    *pipe = fs_pipeline(fs, s_pres_layout, s_sc_format, VK_SAMPLE_COUNT_1_BIT);
    *format = s_sc_format;
    if (!*pipe) {
        s_pres = 0;
        return 0;
    }
    fprintf(stderr, "  [VK] filtered present on (%s source)\n", multisampled ? "MSAA" : "resolved");
    return 1;
}

/* Recorded outside rendering, between barriers (surf_resolve). */
static int shader_resolve(VkSurf *x)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkViewport v = { 0, 0, (float)x->pw, (float)x->ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { x->pw, x->ph } };
    if (!sres_init())
        return 0;
    ca.imageView = x->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_sres_pipe);
    vkCmdSetViewport(s_cb, 0, 1, &v);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_sres_sampler;
    ii.imageView = x->ms_view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_sres_layout, 0, 1, &w);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    /* The draw path's pipeline, dynamic state, vertex input and descriptor
     * caches no longer describe the command buffer. */
    s_cb_gen++;
    s_dyn_valid = 0;
    s_sc_cur.extent.width = 0;
    return 1;
}

/* ss_post's way back: the image into every sample of the twin (recorded
 * outside rendering, between barriers -- begin_rendering). */
static VkPipeline s_unres_pipe;
static void ss_unresolve(VkSurf *x)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkViewport v = { 0, 0, (float)x->pw, (float)x->ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { x->pw, x->ph } };
    static const char fs[] =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D t;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }\n";
    x->ss_newer = 0;
    if (!sres_init())
        return;
    if (!s_unres_pipe && !(s_unres_pipe = fs_pipeline(fs, s_sres_layout, VK_FORMAT_B8G8R8A8_UNORM,
                                                       (VkSampleCountFlagBits)s_msaa)))
        return;
    {
        static uint32_t n;
        if (n++ < 8)
            fprintf(stderr, "  [VK] ss_post: %08X copied back into its samples (frame %u)\n", x->va, s_frame);
    }
    ca.imageView = x->ms_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    pp_begin(2, x, (int)s_msaa);
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_unres_pipe);
    vkCmdSetViewport(s_cb, 0, 1, &v);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_sres_sampler;
    ii.imageView = x->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_sres_layout, 0, 1, &w);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    pp_end();
    s_cb_gen++;
    s_dyn_valid = 0;
    s_sc_cur.extent.width = 0;
}

static VkProg *prog_find(uint64_t vkey, const Nv2aPshKey *pk)
{
    int i;
    for (i = s_prog_head[prog_bucket(vkey, pk)]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk))
            return c;
    }
    return NULL;
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *d = NULL;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
            *size = (size_t)n;
        else { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

static void pcache_save_sync(void)
{
    char path[256], tmp[272];
    size_t n = 0;
    void *d;
    FILE *f;
    if (!s_pcache || (getenv("RECOMP_VK_PIPECACHE") && getenv("RECOMP_VK_PIPECACHE")[0] == '0')
        || !cache_path(path, sizeof path, "vkpipecache.bin"))
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    d = malloc(n);
    if (!d || vkGetPipelineCacheData(s_dev, s_pcache, &n, d) != VK_SUCCESS) { free(d); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb"))) {
        int ok = fwrite(d, 1, n, f) == n;
        fclose(f);
        if (ok) {
            remove(path);                  /* FAT: rename does not replace */
            rename(tmp, path);
        }
    }
    free(d);
    s_pipes_unsaved = 0;
}

/* The periodic cache writes from vk_flip (cache_flush every 5 s,
 * pcache_save every 20 s with new pipelines) on a writer thread: on the
 * render thread they stalled a frame by up to ~230 ms (SD card; ftrace,
 * barn fixture). The append buffers are handed over under a lock; the
 * pipeline cache is serialised there too (VkPipelineCache is internally
 * synchronised). Shutdown drains the thread and writes synchronously. */
static pthread_mutex_t s_cw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cw_cv = PTHREAD_COND_INITIALIZER;
static CacheAppend     s_cw_pend[3];
static int             s_cw_pcache, s_cw_busy, s_cw_started, s_cw_stop;
static pthread_t       s_cw_thread;

static void *cache_writer(void *arg)
{
    (void)arg;
#if defined(__SWITCH__)
    {   /* lowest priority, any application core: it must never take the
         * render thread's core while that one has work (a new thread starts
         * on its creator's core -- 67-121 ms flip stalls in the barn replay) */
        extern uint32_t svcSetThreadPriority(uint32_t handle, uint32_t prio);
        extern uint32_t svcSetThreadCoreMask(uint32_t handle, int32_t ideal, uint32_t mask);
        svcSetThreadPriority(0xFFFF8000u, 0x3F);
        svcSetThreadCoreMask(0xFFFF8000u, 2, 0x7);
    }
#endif
    pthread_mutex_lock(&s_cw_mu);
    for (;;) {
        CacheAppend take[3];
        int pc, i;
        while (!s_cw_stop && !s_cw_pcache && !s_cw_pend[0].n && !s_cw_pend[1].n && !s_cw_pend[2].n)
            pthread_cond_wait(&s_cw_cv, &s_cw_mu);
        if (s_cw_stop && !s_cw_pcache && !s_cw_pend[0].n && !s_cw_pend[1].n && !s_cw_pend[2].n)
            break;
        memcpy(take, s_cw_pend, sizeof take);
        for (i = 0; i < 3; i++) {           /* the writer owns these buffers now */
            s_cw_pend[i].d = NULL;
            s_cw_pend[i].n = s_cw_pend[i].cap = 0;
        }
        pc = s_cw_pcache;
        s_cw_pcache = 0;
        s_cw_busy = 1;
        pthread_mutex_unlock(&s_cw_mu);
        for (i = 0; i < 3; i++) {
            char path[256];
            FILE *f;
            if (take[i].n && cache_path(path, sizeof path, take[i].name) && (f = fopen(path, "ab"))) {
                fwrite(take[i].d, 1, take[i].n, f);
                fclose(f);
            }
            free(take[i].d);
        }
        if (pc)
            pcache_save_sync();
        pthread_mutex_lock(&s_cw_mu);
        s_cw_busy = 0;
        pthread_cond_broadcast(&s_cw_cv);
    }
    pthread_mutex_unlock(&s_cw_mu);
    return NULL;
}

static int cache_writer_start(void)
{
    pthread_attr_t at;
    if (s_cw_started)
        return s_cw_started > 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u << 10);
    s_cw_stop = 0;
    s_cw_started = pthread_create(&s_cw_thread, &at, cache_writer, NULL) == 0 ? 1 : -1;
    pthread_attr_destroy(&at);
    return s_cw_started > 0;
}

/* Wait until the writer has nothing queued or in hand (shutdown). */
static void cache_writer_drain(void)
{
    if (s_cw_started <= 0)
        return;
    pthread_mutex_lock(&s_cw_mu);
    while (s_cw_busy || s_cw_pcache || s_cw_pend[0].n || s_cw_pend[1].n || s_cw_pend[2].n)
        pthread_cond_wait(&s_cw_cv, &s_cw_mu);
    pthread_mutex_unlock(&s_cw_mu);
}

static void cache_writer_shutdown(void)
{
    if (s_cw_started <= 0)
        return;
    cache_writer_drain();
    pthread_mutex_lock(&s_cw_mu);
    s_cw_stop = 1;
    pthread_cond_broadcast(&s_cw_cv);
    pthread_mutex_unlock(&s_cw_mu);
    pthread_join(s_cw_thread, NULL);
    s_cw_started = 0;
    fprintf(stderr, "  [VK] cache writer stopped\n");
}

static void cache_flush(void)
{
    int i;
    if (!cache_writer_start()) {
        cache_flush_sync();
        return;
    }
    pthread_mutex_lock(&s_cw_mu);
    for (i = 0; i < 3; i++) {
        CacheAppend *a = &s_app[i], *q = &s_cw_pend[i];
        if (!a->n)
            continue;
        q->name = a->name;
        if (!q->n) {                        /* hand the buffer over */
            uint8_t *d = q->d;
            size_t cap = q->cap;
            q->d = a->d; q->n = a->n; q->cap = a->cap;
            a->d = d; a->cap = cap;
        } else {
            size_t need = q->n + a->n;
            if (need > q->cap) {
                uint8_t *d = (uint8_t *)realloc(q->d, need);
                if (!d) { a->n = 0; continue; }
                q->d = d;
                q->cap = need;
            }
            memcpy(q->d + q->n, a->d, a->n);
            q->n = need;
        }
        a->n = 0;
    }
    pthread_cond_broadcast(&s_cw_cv);
    pthread_mutex_unlock(&s_cw_mu);
}

static void pcache_save(void)
{
    if (!cache_writer_start()) {
        pcache_save_sync();
        return;
    }
    pthread_mutex_lock(&s_cw_mu);
    s_cw_pcache = 1;
    s_pipes_unsaved = 0;
    pthread_cond_broadcast(&s_cw_cv);
    pthread_mutex_unlock(&s_cw_mu);
}

static void prewarm(void)
{
    char path[256];
    FILE *f;
    uint64_t t0 = hz_now();
    uint32_t nprog = 0, npipe = 0, bad = 0, cut = 0;
    ProgRec rec;
    PipeRec pr;
    /* RECOMP_VK_PREWARM_MS (default 2000): prewarm runs on the push-buffer
     * thread before the title's first frame. On the console NAK compiles a
     * program in ~75 ms even from cached SPIR-V; GL's 348-program list took
     * 26 s and the title never drew a frame afterwards (stuck waiting on the
     * GPU in the boot movie). Whatever is left compiles at first use. */
    static double budget_ms = -1;
    if (budget_ms < 0) {
        const char *e = getenv("RECOMP_VK_PREWARM_MS");
        budget_ms = e && *e ? atof(e) : (s_prewarm_early ? 8000.0 : 2000.0);
    }

    spv_load();
    s_prewarming = 1;
    if (cache_path(path, sizeof path, "progcache.bin") && (f = fopen(path, "rb"))) {
        while (fread(&rec, sizeof rec, 1, f) == 1) {
            Nv2aRawBatch bb;
            if (rec.magic != PROG_REC_MAGIC || rec.slots > 136) { bad++; break; }
            if ((hz_now() - t0) / 1e6 > budget_ms) { cut = 1; break; }
            prog_rec_batch(&rec, &bb, nprog);
            if (prog_get(&bb, &rec.pk))
                nprog++;
        }
        fclose(f);
    }
    if (cache_path(path, sizeof path, "vkpipes.bin") && (f = fopen(path, "rb"))) {
        while (fread(&pr, sizeof pr, 1, f) == 1) {
            VkProg *p;
            PipeKey k;
            if (pr.magic != PIPE_REC_MAGIC) { bad++; break; }
            if ((hz_now() - t0) / 1e6 > budget_ms) { cut = 1; break; }
            if (!(p = prog_find(pr.vkey, &pr.pk)))
                continue;
            memset(&k, 0, sizeof k);
            k.prog = (int)(p - s_prog);
            k.blend = pr.blend; k.bsrc = pr.bsrc; k.bdst = pr.bdst; k.beq = pr.beq;
            k.cmask = pr.cmask; k.depth = pr.depth; k.topo = pr.topo;
            k.samples = pr.pad ? pr.pad : (uint32_t)s_msaa;
            if (pipe_get(&k))
                npipe++;
        }
        fclose(f);
    }
    s_prewarming = 0;
    fprintf(stderr, "  [VK] prewarm: %u programs (%u SPIR-V from cache), %u pipelines in %.0f ms%s%s\n",
            nprog, s_spv_hits, npipe, (hz_now() - t0) / 1e6, bad ? " (a file ends in a bad record)" : "",
            cut ? " (time budget reached, RECOMP_VK_PREWARM_MS)" : "");
    if (s_pipes_unsaved)
        pcache_save_sync();
}

/* ── Context ──────────────────────────────────────────────────────── */

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nf = 16, nm = 8, i;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pd, s_surface, &caps) != VK_SUCCESS)
        return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_pd, s_surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_pd, s_surface, &nm, modes);
    s_sc_format = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_sc_format = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    /* The game paces itself (vblank emulation): presenting must not block
     * it. Horizon's immediate mode does not tear (nvnflinger composes). */
    for (i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    for (i = 0; i < nm && mode == VK_PRESENT_MODE_FIFO_KHR; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    s_sc_extent = caps.currentExtent;
    if (s_sc_extent.width == 0xFFFFFFFFu) {
        s_sc_extent.width = 1280;
        s_sc_extent.height = 720;
    }
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s_sc_format;
    ci.imageExtent = s_sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s_swapchain;
    {
        VkSwapchainKHR sc;
        if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &sc) != VK_SUCCESS)
            return 0;
        if (s_swapchain)
            vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
        s_swapchain = sc;
    }
    s_sc_count = 8;
    vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    for (i = 0; i < 8; i++) {   /* views for the multisampled present pass */
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        if (s_sc_views[i])
            vkDestroyImageView(s_dev, s_sc_views[i], NULL);
        s_sc_views[i] = VK_NULL_HANDLE;
        if (i >= s_sc_count)
            continue;
        vi.image = s_sc_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_sc_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &s_sc_views[i]) != VK_SUCCESS)
            s_sc_views[i] = VK_NULL_HANDLE;
    }
    fprintf(stderr, "  [VK] swapchain %ux%u, %u images, format %d, present mode %d\n",
            s_sc_extent.width, s_sc_extent.height, s_sc_count, s_sc_format, mode);
    return 1;
}

static int ready(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[8];
    uint32_t niext = 0, n, i;
    VkPhysicalDevice pds[8];
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[8];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceFeatures have;
    const char *dext[4];
    uint32_t ndext = 0;
    const char *layers[1];
    uint32_t nlayers = 0;

    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    {
        const char *e = getenv("RECOMP_VK_TRACE");
        s_vtrace = e ? atoi(e) : 0;
    }
    nv2a_shader_vk = 1;
    glslang_initialize_process();

    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(__SWITCH__)
    iext[niext++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
    {
        const char *e = getenv("RECOMP_VK_HEADLESS");
        const char *drv = getenv("SDL_VIDEODRIVER");
        s_headless = (e && *e == '1') || (drv && !strcmp(drv, "offscreen"));
        if (!s_headless) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                s_headless = 1;
            } else {
                s_win = SDL_CreateWindow("NV2A (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                if (!s_win) {
                    s_headless = 1;
                } else {
                    unsigned int ne = 0;
                    const char *names[8];
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, NULL);
                    if (ne > 6) ne = 6;
                    SDL_Vulkan_GetInstanceExtensions(s_win, &ne, names);
                    niext = 0;
                    for (i = 0; i < ne; i++)
                        iext[niext++] = names[i];
                }
            }
        }
        if (s_headless)
            niext = 0;
    }
    {
        const char *v = getenv("RECOMP_VK_VALIDATION");
        if (v && *v == '1')
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    }
#endif
    ai.pApplicationName = "conkerx";
    ai.pEngineName = "xboxrecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    ici.enabledLayerCount = nlayers;
    ici.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed\n");
        return 0;
    }
    n = 8;
    if (vkEnumeratePhysicalDevices(s_inst, &n, pds) != VK_SUCCESS || !n) {
        LOGE("no Vulkan device\n");
        return 0;
    }
    s_pd = pds[0];
    vkGetPhysicalDeviceProperties(s_pd, &pp);
    fprintf(stderr, "  [VK] %s, Vulkan %u.%u.%u\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    s_ubo_align = pp.limits.minUniformBufferOffsetAlignment;
    s_tq_period_ns = pp.limits.timestampPeriod;
#ifdef __SWITCH__
    /* NVK reports 1 ns, but GM20B's timer counts slower: frames waited on
     * right after submit (RECOMP_VK_SYNC=1) took 32.5 ms by the wall clock
     * against 19.7 ms of ticks, ~5/3 -- every GPU figure before 2026-10-09
     * was 40% low. RECOMP_VK_TS_PERIOD overrides. */
    {
        const char *e = getenv("RECOMP_VK_TS_PERIOD");
        if (e && *e) s_tq_period_ns = atof(e);
        else if (s_tq_period_ns == 1.0) s_tq_period_ns = 5.0 / 3.0;
    }
#endif
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        const char *ms = getenv("RECOMP_GL_MSAA");
        /* Unset: the platform's choice, as nv2a_gl.c (Switch: 2.25x docked,
         * 1.5x handheld, 4x MSAA). Vulkan had been rendering 1x, no MSAA. */
        double k = e ? strtod(e, NULL) : (xbox_platform_gl_scale ? xbox_platform_gl_scale() : 1.0);
        int want = ms ? atoi(ms) : (xbox_platform_gl_msaa ? xbox_platform_gl_msaa() : 0);
        VkSampleCountFlags ok = pp.limits.framebufferColorSampleCounts &
                                pp.limits.framebufferDepthSampleCounts &
                                pp.limits.framebufferStencilSampleCounts;
        s_msaa = VK_SAMPLE_COUNT_1_BIT;
        while (want >= 2) {
            VkSampleCountFlagBits c = want >= 8 ? VK_SAMPLE_COUNT_8_BIT
                                    : want >= 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
            if (ok & c) { s_msaa = c; break; }
            want = (int)c / 2;
        }
        if (s_msaa != VK_SAMPLE_COUNT_1_BIT)
            fprintf(stderr, "  [VK] %dx MSAA (RECOMP_GL_MSAA)\n", (int)s_msaa);
        uint32_t m = pp.limits.maxImageDimension2D;
        if (pp.limits.maxFramebufferWidth < m) m = pp.limits.maxFramebufferWidth;
        if (pp.limits.maxFramebufferHeight < m) m = pp.limits.maxFramebufferHeight;
        s_max_size = m ? m : 4096;
        if (!(k > 0.0)) k = 1.0;                    /* unset, 0 or garbage */
        s_scale = k < 0.5 ? 0.5 : k > 4.0 ? 4.0 : k;
        if (s_scale != 1.0)
            fprintf(stderr, "  [VK] rendering at %gx (RECOMP_GL_SCALE), surfaces up to %u\n",
                    s_scale, s_max_size);
    }
    if (s_ubo_align < 16) s_ubo_align = 16;
    vkGetPhysicalDeviceMemoryProperties(s_pd, &s_memprops);
    vkGetPhysicalDeviceFeatures(s_pd, &have);
    s_have_bc = have.textureCompressionBC;
    s_have_depth_clamp = have.depthClamp;
    s_have_large_points = have.largePoints;
    s_have_aniso = have.samplerAnisotropy;
    {   /* Vertex programs size their points through gl_PointSize (oPts),
         * in the title's pixels: scale with the surfaces (nv2a_gl.c). */
        extern float nv2a_point_scale;
        nv2a_point_scale = (float)s_scale;
    }
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            s_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        {
            const char *e = getenv("RECOMP_GL_DXT");
            if (e && *e == '0')
                s_have_bc = 0;
        }
    }

    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &n, qf);
    for (s_qfam = 0; s_qfam < n; s_qfam++)
        if (qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (s_qfam == n) {
        LOGE("no graphics queue\n");
        return 0;
    }

#if defined(__SWITCH__)
    {
        NWindow *win = nwindowGetDefault();
        VkViSurfaceCreateInfoNN sci = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
        nwindowSetDimensions(win, 1280, 720);
        sci.window = win;
        if (vkCreateViSurfaceNN(s_inst, &sci, NULL, &s_surface) != VK_SUCCESS) {
            LOGE("vkCreateViSurfaceNN failed\n");
            return 0;
        }
    }
#else
    if (!s_headless && !SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surface)) {
        LOGE("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        s_headless = 1;
    }
#endif

    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    f13.dynamicRendering = VK_TRUE;
    {   /* RECOMP_PSH_DEMOTE=1: alpha test / clip planes demote (gl_psh.c) */
        const char *e = getenv("RECOMP_PSH_DEMOTE");
        VkPhysicalDeviceVulkan13Features q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 q2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        q2.pNext = &q;
        vkGetPhysicalDeviceFeatures2(s_pd, &q2);
        if (e && *e == '1' && q.shaderDemoteToHelperInvocation) {
            f13.shaderDemoteToHelperInvocation = VK_TRUE;
            nv2a_shader_demote = 1;
            fprintf(stderr, "  [VK] alpha test demotes (RECOMP_PSH_DEMOTE)\n");
        }
    }
    fvi.vertexInputDynamicState = VK_TRUE;
    feds.extendedDynamicState = VK_TRUE;
    f13.pNext = &fvi;
    f2.pNext = &f13;
    f2.features.textureCompressionBC = s_have_bc ? VK_TRUE : VK_FALSE;
    f2.features.depthClamp = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    /* Without it point sizes clamp to 1 (the slingshot pellets) */
    f2.features.largePoints = s_have_large_points ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = s_have_aniso ? VK_TRUE : VK_FALSE;
    if (!s_headless)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    (void)feds;
    if (vkCreateDevice(s_pd, &dci, NULL, &s_dev) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed (needs push descriptors, dynamic vertex input, Vulkan 1.3)\n");
        return 0;
    }
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
#define GET(p, name) p = (void *)vkGetDeviceProcAddr(s_dev, name); if (!p) { LOGE("missing %s\n", name); return 0; }
    GET(p_push_desc, "vkCmdPushDescriptorSetKHR");
    GET(p_vertex_input, "vkCmdSetVertexInputEXT");
    GET(p_begin_rendering, "vkCmdBeginRendering");
    GET(p_end_rendering, "vkCmdEndRendering");
    GET(p_cull_mode, "vkCmdSetCullMode");
    GET(p_front_face, "vkCmdSetFrontFace");
    GET(p_topology, "vkCmdSetPrimitiveTopology");
    GET(p_depth_test, "vkCmdSetDepthTestEnable");
    GET(p_depth_write, "vkCmdSetDepthWriteEnable");
    GET(p_depth_op, "vkCmdSetDepthCompareOp");
    GET(p_stencil_test, "vkCmdSetStencilTestEnable");
    GET(p_stencil_op, "vkCmdSetStencilOp");
#undef GET

    if (!s_headless && !create_swapchain()) {
        LOGE("swapchain creation failed; running headless\n");
        s_headless = 1;
    }

    {
        VkDescriptorSetLayoutBinding b[6];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        VkDescriptorSetLayout sets[2];
        if (s_dynubo < 0) { const char *e = getenv("RECOMP_VK_DYNUBO"); s_dynubo = !(e && *e == '0'); }
        memset(b, 0, sizeof b);
        for (i = 0; i < 6; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].descriptorType = i < 2 ? (s_dynubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                    : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = s_dynubo ? 4 : 6;
        lci.pBindings = s_dynubo ? b + 2 : b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS)
            return 0;
        sets[0] = s_dsl;
        pli.setLayoutCount = 1;
        if (s_dynubo) {
            VkDescriptorSetLayoutCreateInfo uci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            uci.bindingCount = 2;
            uci.pBindings = b;
            if (vkCreateDescriptorSetLayout(s_dev, &uci, NULL, &s_dsl_ubo) != VK_SUCCESS)
                return 0;
            sets[1] = s_dsl_ubo;
            pli.setLayoutCount = 2;
        }
        pli.pSetLayouts = sets;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_layout) != VK_SUCCESS)
            return 0;
        {
            char path[256];
            size_t n = 0;
            /* RECOMP_VK_PIPECACHE=0: start the driver cache empty (the other
             * caches stay on) -- isolates the cache-on boot hang on the console */
            const char *pc = getenv("RECOMP_VK_PIPECACHE");
            int pc_off = pc && pc[0] == '0';
            void *d = !pc_off && cache_path(path, sizeof path, "vkpipecache.bin") ? read_file(path, &n) : NULL;
            pci.initialDataSize = n;
            pci.pInitialData = d;
            if (vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache) != VK_SUCCESS) {
                pci.initialDataSize = 0;       /* another driver's or a damaged file */
                pci.pInitialData = NULL;
                vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache);
            }
            s_pcache_loaded=pci.initialDataSize!=0;
            if (n)
                fprintf(stderr, "  [VK] pipeline cache: %u KB from %s\n", (unsigned)(n >> 10), path);
            free(d);
        }
    }

    for (i = 0; i < VK_FRAMES; i++) {
        Frame *f = &s_fr[i];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements rq;
        cpi.queueFamilyIndex = s_qfam;
        vkCreateCommandPool(s_dev, &cpi, NULL, &f->pool);
        cai.commandPool = f->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(s_dev, &cai, &f->cb);
        vkAllocateCommandBuffers(s_dev, &cai, &f->pcb);
        vkCreateFence(s_dev, &fci, NULL, &f->fence);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->acquired);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->rendered);
        bci.size = RING_BYTES;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &f->ring) != VK_SUCCESS)
            return 0;
        vkGetBufferMemoryRequirements(s_dev, f->ring, &rq);
        {   /* RECOMP_VK_RING_MEM=<VkMemoryPropertyFlags, hex>: experiment */
            const char *e = getenv("RECOMP_VK_RING_MEM");
            VkMemoryPropertyFlags want = e && *e ? (VkMemoryPropertyFlags)strtoul(e, NULL, 16)
                : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            uint32_t k;
            if (f == &s_fr[0]) {
                for (k = 0; k < s_memprops.memoryTypeCount; k++)
                    fprintf(stderr, "  [VK] memory type %u: flags %03X heap %u%s\n", k,
                            (unsigned)s_memprops.memoryTypes[k].propertyFlags, s_memprops.memoryTypes[k].heapIndex,
                            k == mem_type(rq.memoryTypeBits, want) ? "  <- frame ring" : "");
            }
            f->ring_mem = alloc_mem(rq, want);
        }
        if (!f->ring_mem || vkBindBufferMemory(s_dev, f->ring, f->ring_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s_dev, f->ring_mem, 0, RING_BYTES, 0, (void **)&f->ring_ptr) != VK_SUCCESS) {
            LOGE("frame ring allocation failed\n");
            return 0;
        }
        bci.size = IRING_BYTES;
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &f->iring) != VK_SUCCESS)
            return 0;
        vkGetBufferMemoryRequirements(s_dev, f->iring, &rq);
        f->iring_mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!f->iring_mem || vkBindBufferMemory(s_dev, f->iring, f->iring_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s_dev, f->iring_mem, 0, IRING_BYTES, 0, (void **)&f->iring_ptr) != VK_SUCCESS) {
            LOGE("index ring allocation failed\n");
            return 0;
        }
    }
    if (s_dynubo) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * VK_FRAMES };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorSetLayout ls[VK_FRAMES];
        dpi.maxSets = VK_FRAMES;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(s_dev, &dpi, NULL, &s_ubo_pool) != VK_SUCCESS)
            return 0;
        for (i = 0; i < VK_FRAMES; i++) ls[i] = s_dsl_ubo;
        dai.descriptorPool = s_ubo_pool;
        dai.descriptorSetCount = VK_FRAMES;
        dai.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(s_dev, &dai, s_ubo_set) != VK_SUCCESS)
            return 0;
        for (i = 0; i < VK_FRAMES; i++) {
            VkDescriptorBufferInfo bi[2];
            VkWriteDescriptorSet w[2];
            int k;
            memset(w, 0, sizeof w);
            bi[0].buffer = bi[1].buffer = s_fr[i].ring;
            bi[0].offset = bi[1].offset = 0;
            bi[0].range = sizeof(VsBlock);
            bi[1].range = sizeof(FsBlock);
            for (k = 0; k < 2; k++) {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[k].dstSet = s_ubo_set[i];
                w[k].dstBinding = (uint32_t)k;
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w[k].pBufferInfo = &bi[k];
            }
            vkUpdateDescriptorSets(s_dev, 2, w, 0, NULL);
        }
    }
    if (s_tq_period_ns > 0) {
        VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = VK_FRAMES * 2;
        if (vkCreateQueryPool(s_dev, &qpi, NULL, &s_tq) != VK_SUCCESS)
            s_tq = VK_NULL_HANDLE;
    }
    pp_init();
    s_fi = 0;
    frame_begin();
    if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_img, &s_dummy_view, &s_dummy_mem))
        return 0;
    clear_image_color(s_dummy_img, 0, 0, 0, 0);
    if (!make_image_ex(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, 1, &s_dummy_cube_img, &s_dummy_cube_view,
                       &s_dummy_cube_mem))
        return 0;
    clear_image_color(s_dummy_cube_img, 0, 0, 0, 0);
    s_state = 1;
    fprintf(stderr, "  [VK] ready%s: BC %s, depth %s, depth clamp %s, UBO align %u\n",
            s_headless ? " (headless)" : "", s_have_bc ? "yes" : "no",
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            s_have_depth_clamp ? "yes" : "no", (unsigned)s_ubo_align);
    prewarm();
    return 1;
}

/* Submit what is recorded and wait for it, then keep recording in the same
 * frame (ring full). */
static void flush_frame_wait(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    end_rendering();
    s_rt = NULL;
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    vkQueueSubmit(s_queue, 1, &si, s_f->fence);
    nv2a_pipe_state(1, 2);
    vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
    nv2a_pipe_state(1, 0);
    vkResetFences(s_dev, 1, &s_f->fence);
    garbage_free(s_f);
    s_f->ring_off = 0;
    s_f->ioff = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cb, &bi);
    s_cb_gen++;
    s_dyn_valid = 0;
    pp_frame_start(1);
    if (s_tq) {     /* the frame restarts its GPU clock in the new command buffer */
        vkCmdResetQueryPool(s_cb, s_tq, (uint32_t)s_fi * 2, 2);
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tq, (uint32_t)s_fi * 2);
    }
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }
static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static VkCompareOp cmp_op(uint32_t v, uint32_t dflt)
{
    if (!v) v = dflt;
    if (v < 0x200 || v > 0x207) v = dflt;
    return (VkCompareOp)(v - 0x200);
}

static VkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;         /* KEEP, and 0 (never set) */
    }
}

/* Dynamic state for a draw, set only when it changed. */
typedef struct {
    uint32_t cull, cullmode, front, topo;
    uint32_t dtest, dwrite, dop;
    uint32_t stest, sfail, szfail, spass, sop, smask, swrite, sref;
    uint32_t bcolor;
    uint32_t zb_const, zb_slope;        /* SET_POLYGON_OFFSET_BIAS / _SCALE_FACTOR bits */
} DynState;
static DynState s_dyn;

static void apply_dynamic(const uint32_t *r, int has_depth, VkPrimitiveTopology topo)
{
    DynState d;
    memset(&d, 0, sizeof d);
    d.cull = r[0x308 / 4] != 0;
    d.cullmode = r[0x39C / 4];
    d.front = r[0x3A0 / 4];
    d.topo = (uint32_t)topo;
    d.dtest = has_depth && r[0x30C / 4];
    d.dwrite = has_depth && r[0x35C / 4];
    d.dop = r[0x354 / 4];
    d.stest = has_depth && r[0x32C / 4];
    if (d.stest) {
        d.sfail = r[0x370 / 4]; d.szfail = r[0x374 / 4]; d.spass = r[0x378 / 4];
        d.sop = r[0x364 / 4]; d.smask = r[0x36C / 4] & 0xFF; d.swrite = r[0x360 / 4] & 0xFF;
        d.sref = r[0x368 / 4] & 0xFF;
    }
    d.bcolor = r[0x34C / 4];
    /* SET_POLYGON_OFFSET_FILL_ENABLE (0x338) with SCALE_FACTOR (0x384) and BIAS
     * (0x388), as glPolygonOffset(factor, units). Conker draws its projected
     * character shadows coplanar with the floor at offset (-4, -16); without
     * it they z-fought the floor and flickered as the camera and the enemies
     * moved (fixtures: play19 shadowflk2.fixz). */
    if (has_depth && r[0x338 / 4]) {
        d.zb_const = r[0x388 / 4];
        d.zb_slope = r[0x384 / 4];
    } else {
        d.zb_const = d.zb_slope = 0;
    }
    if (s_dyn_valid && !memcmp(&d, &s_dyn, sizeof d))
        return;
    if (!s_dyn_valid || d.cull != s_dyn.cull || d.cullmode != s_dyn.cullmode) {
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        if (d.cull)
            cm = d.cullmode == 0x404 ? VK_CULL_MODE_FRONT_BIT
               : d.cullmode == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        p_cull_mode(s_cb, cm);
    }
    if (!s_dyn_valid || d.front != s_dyn.front)
        /* NV2A winding is as seen on screen, rows top-first -- Vulkan's
         * framebuffer orientation, so CW is CW. nv2a_gl swaps because its
         * window y runs the other way. Swapped here too, every culled draw
         * (29 a frame in the main menu: headlight glass, grille) lost its
         * front faces and kept its back ones. */
    {
        static int swap = -1;          /* RECOMP_VK_FRONT_SWAP=1: GL's winding (diagnostic) */
        if (swap < 0) { const char *e = getenv("RECOMP_VK_FRONT_SWAP"); swap = e && *e == '1'; }
        p_front_face(s_cb, (d.front == 0x901) != swap ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                      : VK_FRONT_FACE_CLOCKWISE);
    }
    if (!s_dyn_valid || d.topo != s_dyn.topo)
        p_topology(s_cb, topo);
    {   /* RECOMP_VK_NO_ZTEST=1 / RECOMP_VK_NO_STEST=1 (diagnostic) */
        static int nz = -1, ns = -1;
        if (nz < 0) { nz = getenv("RECOMP_VK_NO_ZTEST") != NULL; ns = getenv("RECOMP_VK_NO_STEST") != NULL; }
        if (nz) d.dtest = 0;
        if (ns) d.stest = 0;
    }
    if (!s_dyn_valid || d.dtest != s_dyn.dtest) p_depth_test(s_cb, d.dtest);
    if (!s_dyn_valid || d.dwrite != s_dyn.dwrite) p_depth_write(s_cb, d.dwrite);
    if (!s_dyn_valid || d.dop != s_dyn.dop) p_depth_op(s_cb, cmp_op(d.dop, 0x203));
    if (!s_dyn_valid || d.stest != s_dyn.stest) p_stencil_test(s_cb, d.stest);
    if (!s_dyn_valid || memcmp(&d.sfail, &s_dyn.sfail, 7 * sizeof(uint32_t))) {
        p_stencil_op(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(d.sfail), stencil_op(d.spass),
                     stencil_op(d.szfail), cmp_op(d.sop, 0x207));
        vkCmdSetStencilCompareMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.smask);
        vkCmdSetStencilWriteMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.swrite);
        vkCmdSetStencilReference(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.sref);
    }
    if (!s_dyn_valid || d.zb_const != s_dyn.zb_const || d.zb_slope != s_dyn.zb_slope) {
        float c, sl;
        memcpy(&c, &d.zb_const, 4);
        memcpy(&sl, &d.zb_slope, 4);
        {   /* RECOMP_VK_ZBIAS_MUL=<k>,<j> (diagnostic): scale constant, slope */
            static float kc = -1, ks = 1;
            if (kc < 0) { const char *e = getenv("RECOMP_VK_ZBIAS_MUL"); kc = e ? (float)atof(e) : 1; ks = e && strchr(e, 0x2C) ? (float)atof(strchr(e, 0x2C) + 1) : 1; }
            c *= kc; sl *= ks;
        }
        vkCmdSetDepthBias(s_cb, c, 0.0f, sl);
    }
    if (!s_dyn_valid || d.bcolor != s_dyn.bcolor) {
        float bc[4];
        argb_vec4(d.bcolor, bc);
        vkCmdSetBlendConstants(s_cb, bc);
    }
    s_dyn = d;
    s_dyn_valid = 1;
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
    for (i = 0; i < 4; i++)          /* TEXTURE_FILTER bits 28-31: signed channels */
        k->tex_sign |= ((r[(0x1B14u + (uint32_t)i * 0x40u) / 4] >> 28) & 0xFu) << (4 * i);
    /* SET_ALPHA_TEST_ENABLE with a function other than ALWAYS: only then
     * does the shader carry the discard (RECOMP_VK_ATEST_ALWAYS=1: always,
     * as before). */
    {
        static int always = -1;
        if (always < 0) { const char *e = getenv("RECOMP_VK_ATEST_ALWAYS"); always = e && *e == '1'; }
        if (always || (r[0x300 / 4] && r[0x33C / 4] != 0x207))
            k->tex_sign |= NV2A_PSH_ATEST;
    }
}

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* NV097 primitive -> Vulkan topology and (possibly rewritten) indices. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static int prim_indices(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n,
                        VkPrimitiveTopology *topo, uint32_t *cls)
{
    uint32_t i, m = 0;
    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return 1;
    case 2: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return 1;
    case 4: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return 1;
    case 5: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return 1;
    case 6: case 9: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return 1;
    case 7: case 10: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; *cls = 2; return 1;
    case 3:                                      /* LINE_LOOP: strip + first */
    case 8: {                                    /* QUADS -> triangles */
        size_t need = b->prim == 3 ? (size_t)b->index_count + 1 : (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return 0; }
        if (b->prim == 3) {
            if (!b->index_count) { *n = 0; return 0; }
            memcpy(s_prim_idx, b->indices, (size_t)b->index_count * 4);
            s_prim_idx[b->index_count] = b->indices[0];
            m = b->index_count + 1;
            *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1;
        } else {
            for (i = 0; i + 3 < b->index_count; i += 4) {
                const uint32_t *q = b->indices + i;
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
            }
            *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2;
        }
        *idx = s_prim_idx;
        *n = m;
        return 1;
    }
    default:
        *n = 0;
        return 0;
    }
}

/* ── Vertices ─────────────────────────────────────────────────────── */

typedef struct { const uint8_t *lo, *hi; VkDeviceSize at; uint64_t sig; uint32_t slot; int hit; } VRun;

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;
    case 2: return 4 * size;
    case 4: return size;
    case 6: return 4;
    default: return 2 * size;
    }
}

static uint32_t vertex_runs(const Nv2aRawBatch *b, uint16_t direct, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + elem_bytes(b->direct[a].type, b->direct[a].size);
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
            if (!(direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j) of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1)) of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

/* The Vulkan vertex format of a direct attribute, or UNDEFINED when the
 * device cannot fetch it (then it is converted to float4 here). */
static VkFormat s_vfmt_cache[8][5];
static int      s_vfmt_known[8][5];

static VkFormat vertex_format(uint32_t type, uint32_t size)
{
    static const VkFormat f_float[5] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                         VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f_snorm16[5] = { 0, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                           VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    static const VkFormat f_unorm8[5] = { 0, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                          VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat f_sscaled16[5] = { 0, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                             VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    VkFormat f;
    if (size < 1 || size > 4 || type > 7)
        return VK_FORMAT_UNDEFINED;
    if (s_vfmt_known[type][size])
        return s_vfmt_cache[type][size];
    switch (type) {
    case 0: f = VK_FORMAT_B8G8R8A8_UNORM; break;
    case 1: f = f_snorm16[size]; break;
    case 2: f = f_float[size]; break;
    case 4: f = f_unorm8[size]; break;
    case 5: f = f_sscaled16[size]; break;
    case 6: f = size == 1 && !getenv("RECOMP_VK_CMP_CPU") ? VK_FORMAT_R32_SINT : VK_FORMAT_UNDEFINED; break;   /* CMP: unpacked in the shader */
    default: f = VK_FORMAT_UNDEFINED; break;
    }
    if (f != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            if (1)   /* once per (type, size): the result is cached */
                LOGE("vertex format %d (type %u size %u) not supported: converted\n", f, type, size);
            f = VK_FORMAT_UNDEFINED;
        }
    }
    s_vfmt_known[type][size] = 1;
    s_vfmt_cache[type][size] = f;
    return f;
}

/* One attribute of vertex v, as the NV2A reads it, into float4. */
static void attr_to_float4(const Nv2aRawBatch *b, uint32_t a, uint32_t v, float out[4])
{
    const uint8_t *p = b->direct[a].ptr + (size_t)v * b->direct[a].stride;
    uint32_t size = b->direct[a].size, c;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (b->direct[a].type) {
    case 0:                                          /* D3DCOLOR: B,G,R,A */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1:
        for (c = 0; c < size && c < 4; c++) {
            int16_t s; memcpy(&s, p + 2 * c, 2);
            out[c] = s / 32767.0f < -1.0f ? -1.0f : s / 32767.0f;
        }
        break;
    case 2:
        for (c = 0; c < size && c < 4; c++) memcpy(&out[c], p + 4 * c, 4);
        break;
    case 4:
        for (c = 0; c < size && c < 4; c++) out[c] = p[c] / 255.0f;
        break;
    case 6: {                                        /* CMP (RECOMP_VK_CMP_CPU=1) */
        int32_t w;
        memcpy(&w, p, 4);
        out[0] = (float)((int32_t)((uint32_t)w << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)((uint32_t)w << 10) >> 21) / 1023.0f;
        out[2] = (float)(w >> 22) / 511.0f;
        break;
    }
    default:
        for (c = 0; c < size && c < 4; c++) { int16_t s; memcpy(&s, p + 2 * c, 2); out[c] = (float)s; }
        break;
    }
}

/* CPU time by renderer stage (nv2a_vk_cpu_stats; the fixture replay prints
 * it per frame): where a frame's milliseconds go, for the performance pass. */
enum { SEC_PROG, SEC_TEX, SEC_PIPE, SEC_PASS, SEC_UNIF, SEC_DESC, SEC_VPREP, SEC_VCOPY,
       SEC_VCONV, SEC_VBIND, SEC_VERT, SEC_DRAW, SEC_CLEAR, SEC_FLIP, SEC_N };
static const char *const s_sec_name[SEC_N] = { "key+program", "textures", "pipeline",
    "pass+state", "uniforms", "descriptors", "vtx-prep", "vtx-copy", "vtx-convert", "vtx-bind",
    "indices", "draw call", "clears", "flip+wait" };
static uint64_t s_sec_ns[SEC_N], s_sec_t;

static uint32_t s_sec_frames;
#define SEC(k) do { uint64_t now_ = hz_now(); s_sec_ns[k] += now_ - s_sec_t; s_sec_t = now_; } while (0)

extern uint64_t nv2a_fixture_mem_ns __attribute__((weak));
static uint64_t s_ft_sec0[SEC_N], s_ft_sec[FT_N][SEC_N], s_ft_mem0, s_ft_mem[FT_N];
static void ft_secs(uint32_t frame, int end)
{
    int i;
    if (&nv2a_fixture_mem_ns) {
        if (!end) s_ft_mem0 = nv2a_fixture_mem_ns;
        else s_ft_mem[frame % FT_N] = nv2a_fixture_mem_ns - s_ft_mem0;
    }
    for (i = 0; i < SEC_N; i++) {
        if (!end)
            s_ft_sec0[i] = s_sec_ns[i];
        else
            s_ft_sec[frame % FT_N][i] = s_sec_ns[i] >= s_ft_sec0[i] ? s_sec_ns[i] - s_ft_sec0[i] : 0;
    }
}
static void ft_secs_print(uint32_t frame, double rec_ms)
{
    char line[1024];
    size_t n = 0;
    double sum = 0;
    int i;
    for (i = 0; i < SEC_N; i++) {
        double ms = s_ft_sec[frame % FT_N][i] / 1e6;
        sum += ms;
        n += (size_t)snprintf(line + n, sizeof line - n, " %s %.2f", s_sec_name[i], ms);
    }
    fprintf(stderr, "[ftrace] f%u draws %u: timed %.2f replay-mem %.2f other-untimed %.2f |%s\n", frame,
            s_ft_draws[frame % FT_N], sum, s_ft_mem[frame % FT_N] / 1e6,
            rec_ms - sum - s_ft_mem[frame % FT_N] / 1e6, line);
}

/* Vertex runs already in this frame's ring: Conker draws a model as many
 * index ranges into one vertex buffer, and each draw copied the whole buffer
 * again (11 ms of CPU a frame in the barn fixture). Keyed by address, length
 * and a sampled signature (16 words spread over the run plus both ends), so a
 * buffer rewritten between draws is uploaded again. Cleared per command
 * buffer (s_cb_gen: frame or mid-frame flush). RECOMP_VK_VCACHE=0 disables. */
#define VCACHE_N 512
static struct { const uint8_t *lo; uint32_t len, gen; uint64_t sig; VkDeviceSize at; } s_vc[VCACHE_N];

static uint64_t run_sig(const uint8_t *lo, uint32_t len)
{
    uint64_t h = 1469598103934665603ull ^ len;
    uint32_t i, w;
    if (len >= 16) {
        memcpy(&w, lo, 4); h = (h ^ w) * 1099511628211ull;
        memcpy(&w, lo + len - 4, 4); h = (h ^ w) * 1099511628211ull;
        for (i = 0; i < 16; i++) {
            uint32_t at = (uint32_t)(((uint64_t)(len - 4) * i) / 15) & ~3u;
            memcpy(&w, lo + at, 4);
            h = (h ^ w) * 1099511628211ull;
        }
    } else {
        for (i = 0; i < len; i++)
            h = (h ^ lo[i]) * 1099511628211ull;
    }
    return h;
}

/* What upload_vertices bound, for draw merging: one interleaved run of
 * direct attributes (common stride, nothing converted) at ring offset at. */
static struct {
    int ok, why;
    uint32_t nrun;
    VkDeviceSize at[NV2A_RAW_ATTRS];        /* run i's ring offset */
    uint32_t stride[NV2A_RAW_ATTRS];        /* its attributes' common stride */
    int ofr[NV2A_RAW_ATTRS];                /* attribute -> run */
    uint32_t off[NV2A_RAW_ATTRS];           /* attribute's offset in its run */
} s_um;

static int upload_vertices(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun, a, i, v;
    uint32_t nv = b->vertex_count;
    uint16_t direct = 0, conv, cdirect = 0;
    VkDeviceSize bytes = 0, pack_at, const_at, at, const_abs;
    /* The SET_VERTEX_DATA constants (attributes a draw leaves out): reused
     * while attr_const_gen is unchanged in this command buffer, so their
     * binding offsets stay put and the vertex buffers need no rebind. */
    static uint32_t last_cgen, last_cgen_cb;
    static VkDeviceSize last_const_abs;
    int const_reuse = b->attr_const_gen && last_cgen == b->attr_const_gen && last_cgen_cb == s_cb_gen;
    VkFormat fmt[NV2A_RAW_ATTRS];
    uint8_t *dst;
    VkVertexInputBindingDescription2EXT bind[NV2A_RAW_ATTRS];
    VkVertexInputAttributeDescription2EXT attr[NV2A_RAW_ATTRS];
    VkBuffer bufs[NV2A_RAW_ATTRS];
    VkDeviceSize offs[NV2A_RAW_ATTRS];
    void *map;

    if (!nv)
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        fmt[a] = VK_FORMAT_UNDEFINED;
        if (!(b->attr_direct & (1u << a)))
            continue;
        fmt[a] = vertex_format(b->direct[a].type, b->direct[a].size);
        if (fmt[a] != VK_FORMAT_UNDEFINED)
            direct |= (uint16_t)(1u << a);
        else
            cdirect |= (uint16_t)(1u << a);          /* direct data, converted here */
    }
    conv = b->attr_present & (uint16_t)~direct;
    nrun = vertex_runs(b, direct, run, of_run);
    s_um.ok = 0;
    {
        static int vc_on = -1;
        uint32_t gen0 = s_cb_gen;
        if (vc_on < 0) {
            /* Queued draws point into the queue's ring, whose addresses are
             * reused for other data: an address-keyed cache would be wrong. */
            const char *e = getenv("RECOMP_VK_VCACHE");
            vc_on = !(e && *e == '0') && !nv2a_queue_active();
        }
        for (i = 0; i < nrun; i++) {
            uint32_t len = (uint32_t)(run[i].hi - run[i].lo);
            run[i].sig = vc_on ? run_sig(run[i].lo, len) : 0;
            run[i].slot = (uint32_t)(((uintptr_t)run[i].lo >> 4) ^ len) & (VCACHE_N - 1);
            run[i].hit = vc_on && s_vc[run[i].slot].gen == gen0 && s_vc[run[i].slot].lo == run[i].lo
                      && s_vc[run[i].slot].len == len && s_vc[run[i].slot].sig == run[i].sig;
            if (run[i].hit) {
                run[i].at = s_vc[run[i].slot].at;           /* absolute, in the ring */
            } else {
                run[i].at = bytes;                          /* relative, until allocated */
                bytes += ((VkDeviceSize)len + 15) & ~(VkDeviceSize)15;
            }
        }
        (void)gen0;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += (VkDeviceSize)nv * np * 16;
    const_at = bytes;
    if (!const_reuse)
        bytes += NV2A_RAW_ATTRS * 16;
    {
        uint32_t gen_before = s_cb_gen;
        at = ring_alloc(bytes, 16, &map);
        dst = (uint8_t *)map;
        if (!dst)
            return 0;
        if (s_cb_gen != gen_before) {
            /* The ring was flushed for this allocation: cached runs point into
             * the old command buffer. Forget them and upload everything. */
            for (i = 0; i < VCACHE_N; i++) s_vc[i].gen = 0;
            return upload_vertices(b);
        }
    }
    SEC(SEC_VPREP);
    for (i = 0; i < nrun; i++) {
        if (run[i].hit) {
            s_rep[2]++;
            continue;
        }
        s_rep[3]++;
        s_rep[4] += (uint64_t)(run[i].hi - run[i].lo);
        memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
        run[i].at += at;                                    /* now absolute */
        s_vc[run[i].slot].lo = run[i].lo;
        s_vc[run[i].slot].len = (uint32_t)(run[i].hi - run[i].lo);
        s_vc[run[i].slot].gen = s_cb_gen;
        s_vc[run[i].slot].sig = run[i].sig;
        s_vc[run[i].slot].at = run[i].at;
    }
    SEC(SEC_VCOPY);
    s_rep[5] += (uint64_t)nv * np * 16;
    if (np) {
        /* b->attrs holds the executor's conversions (CMP normals, arrays that
         * run off their buffer): [v][16] slots, or with attrs_packed only the
         * attributes outside attr_direct, [v][k] in slot order. Reading a
         * packed batch as [v][16] gave every skinned character garbage
         * normals -- dark, green-tinted lighting (nv2a_gl.c copies it). */
        uint16_t exec_conv = b->attr_present & (uint16_t)~b->attr_direct;
        uint32_t nk = (uint32_t)__builtin_popcount(exec_conv), kidx[NV2A_RAW_ATTRS];
        float *out = (float *)(dst + pack_at);
        for (i = 0; i < np; i++)
            kidx[i] = b->attrs_packed
                    ? (uint32_t)__builtin_popcount(exec_conv & ((1u << slot[i]) - 1u))
                    : slot[i];
        if (b->attrs_packed && !cdirect && np == nk) {
            /* The executor packed exactly these slots in this order: one copy
             * (the per-vertex, per-slot loop was 4.5 ms a frame). */
            memcpy(out, b->attrs, (size_t)nv * np * 16);
        } else {
            for (v = 0; v < nv; v++)
                for (i = 0; i < np; i++) {
                    float *o = out + ((size_t)v * np + i) * 4;
                    if (cdirect & (1u << slot[i]))
                        attr_to_float4(b, slot[i], v, o);
                    else
                        memcpy(o, b->attrs + ((size_t)v * (b->attrs_packed ? nk : NV2A_RAW_ATTRS)
                                              + kidx[i]) * 4, 16);
                }
        }
    }
    SEC(SEC_VCONV);
    if (const_reuse) {
        const_abs = last_const_abs;
    } else {
        memcpy(dst + const_at, b->attr_const, NV2A_RAW_ATTRS * 16);
        const_abs = at + const_at;
        last_const_abs = const_abs;
        last_cgen = b->attr_const_gen;
        last_cgen_cb = s_cb_gen;
    }

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        memset(&bind[a], 0, sizeof bind[a]);
        memset(&attr[a], 0, sizeof attr[a]);
        bind[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        bind[a].binding = a;
        bind[a].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind[a].divisor = 1;
        attr[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        attr[a].location = a;
        attr[a].binding = a;
        bufs[a] = s_f->ring;
        if (direct & (1u << a)) {
            bind[a].stride = b->direct[a].stride;
            attr[a].format = fmt[a];
            offs[a] = run[of_run[a]].at + (VkDeviceSize)(b->direct[a].ptr - run[of_run[a]].lo);
        } else if (conv & (1u << a)) {
            for (i = 0; i < np && slot[i] != a; i++) { }
            bind[a].stride = np * 16;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + pack_at + i * 16;
        } else {
            /* Absent: the title's SET_VERTEX_DATA* value, stride 0. */
            bind[a].stride = 0;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = const_abs + a * 16;
        }
    }
    {   /* Only what changed: NVK hashes and revalidates the whole vertex input
         * state on every vkCmdSetVertexInputEXT, and its vertex-buffer bind
         * goes through an allocating common wrapper -- both per draw before.
         * Valid within one command buffer (s_cb_gen). */
        static VkVertexInputBindingDescription2EXT last_bind[NV2A_RAW_ATTRS];
        static VkVertexInputAttributeDescription2EXT last_attr[NV2A_RAW_ATTRS];
        static VkDeviceSize last_offs[NV2A_RAW_ATTRS];
        static uint32_t last_gen;
        int same_gen = last_gen == s_cb_gen;
        if (!same_gen || (!(s_exp & 4) && (memcmp(bind, last_bind, sizeof last_bind) || memcmp(attr, last_attr, sizeof last_attr)))) {
            p_vertex_input(s_cb, NV2A_RAW_ATTRS, bind, NV2A_RAW_ATTRS, attr);
            s_rep[0]++;
            memcpy(last_bind, bind, sizeof last_bind);
            memcpy(last_attr, attr, sizeof last_attr);
        }
        if (!same_gen || memcmp(offs, last_offs, sizeof last_offs)) {
            /* Only the bindings that changed, in contiguous ranges (one
             * buffer: the ring). The first changed one onwards rebound ~16
             * bindings on every draw: 3.7 ms a frame through NVK. */
            uint32_t first = 0, end;
            while (first < NV2A_RAW_ATTRS) {
                if (same_gen && offs[first] == last_offs[first]) { first++; continue; }
                end = first + 1;
                while (end < NV2A_RAW_ATTRS && !(same_gen && offs[end] == last_offs[end])) end++;
                vkCmdBindVertexBuffers(s_cb, first, end - first, bufs + first, offs + first);
                s_rep[1]++;
                first = end;
            }
            memcpy(last_offs, offs, sizeof last_offs);
        }
        last_gen = s_cb_gen;
    }
    s_um.why = np ? 2 : cdirect ? 3 : direct != b->attr_present ? 4 : 5;
    if (!np && !cdirect && direct == b->attr_present && nrun) {
        s_um.ok = 1;
        s_um.nrun = nrun;
        for (i = 0; i < nrun; i++) {
            s_um.at[i] = run[i].at;
            s_um.stride[i] = 0;
        }
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (direct & (1u << a)) {
                uint32_t r = (uint32_t)of_run[a], st = b->direct[a].stride;
                s_um.ofr[a] = (int)r;
                s_um.off[a] = (uint32_t)(b->direct[a].ptr - run[r].lo);
                if (!s_um.stride[r]) s_um.stride[r] = st;
                else if (s_um.stride[r] != st) s_um.ok = 0;  /* one stride a run */
                if (!st) s_um.ok = 0;
            }
        if (!s_um.ok && getenv("RECOMP_VK_MERGELOG")) {
            static int nlog;
            if (nlog++ < 6) {
                fprintf(stderr, "[mergelog] refused base: %u runs;", nrun);
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if (direct & (1u << a))
                        fprintf(stderr, " a%u t%u s%u st%u run%d off%ld", a, b->direct[a].type, b->direct[a].size,
                                b->direct[a].stride, of_run[a], (long)(b->direct[a].ptr - run[of_run[a]].lo));
                fputc(10, stderr);
            }
        }
    }
    SEC(SEC_VBIND);
    return 1;
}

/* ── Back end entry points ────────────────────────────────────────── */


int nv2a_vk_cpu_stats(char *out, size_t cap)
{
    size_t n = 0;
    uint64_t tot = 0;
    int i;
    if (!s_sec_frames)
        return 0;
    for (i = 0; i < SEC_N; i++)
        tot += s_sec_ns[i];
    n += (size_t)snprintf(out + n, cap - n, "passes %.1f (MSAA %.1f), uploads %.1f per frame; CPU per frame %.2f ms:",
                          (double)s_passes / s_sec_frames, (double)s_passes_ms / s_sec_frames,
                          (double)s_uploads / s_sec_frames, tot / 1e6 / s_sec_frames);
    n += (size_t)snprintf(out + n, cap - n, " [clear: target %.2f begin %.2f cmd %.2f ready %.2f;"
                          " all targets: surf_get %.2f depth_get %.2f]",
                          s_clr_ns[0] / 1e6 / s_sec_frames, s_clr_ns[1] / 1e6 / s_sec_frames,
                          s_clr_ns[2] / 1e6 / s_sec_frames, s_clr_ns[3] / 1e6 / s_sec_frames,
                          s_clr_ns[4] / 1e6 / s_sec_frames, s_clr_ns[5] / 1e6 / s_sec_frames);
    memset(s_clr_ns, 0, sizeof s_clr_ns);
    n += (size_t)snprintf(out + n, cap - n, " [flip: submit %.2f present %.2f fence wait %.2f sync wall %.2f; timestamp period %.4f ns]",
                          s_flp_ns[0] / 1e6 / s_sec_frames, s_flp_ns[1] / 1e6 / s_sec_frames,
                          s_flp_ns[2] / 1e6 / s_sec_frames, s_flp_ns[3] / 1e6 / s_sec_frames, s_tq_period_ns);
    memset(s_flp_ns, 0, sizeof s_flp_ns);
    n += (size_t)snprintf(out + n, cap - n, " [geometry: %.0f vertices, %.0f indices per frame]",
                          (double)s_nverts / s_sec_frames, (double)s_nidx / s_sec_frames);
    n += (size_t)snprintf(out + n, cap - n, " [index span %.0f; indices by prim 1..10: %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f]",
                          (double)s_nspan / s_sec_frames, s_nprim[1] / (double)s_sec_frames, s_nprim[2] / (double)s_sec_frames,
                          s_nprim[3] / (double)s_sec_frames, s_nprim[4] / (double)s_sec_frames, s_nprim[5] / (double)s_sec_frames,
                          s_nprim[6] / (double)s_sec_frames, s_nprim[7] / (double)s_sec_frames, s_nprim[8] / (double)s_sec_frames,
                          s_nprim[9] / (double)s_sec_frames, s_nprim[10] / (double)s_sec_frames);
    memset(s_nprim, 0, sizeof s_nprim);
    n += (size_t)snprintf(out + n, cap - n, " [unique vertices %.0f, degenerate tris %.0f]",
                          (double)s_nuniq / s_sec_frames, (double)s_ndegen / s_sec_frames);
    s_nuniq = s_ndegen = 0;
    if (s_vps_on > 0 && s_nvps) {
        uint32_t k, j, best;
        for (j = 0; j < 12 && j < s_nvps; j++) {
            best = j;
            for (k = j + 1; k < s_nvps; k++) if (s_vps[k].uniq > s_vps[best].uniq) best = k;
            if (best != j) { __typeof__(s_vps[0]) t = s_vps[j]; s_vps[j] = s_vps[best]; s_vps[best] = t; }
            fprintf(stderr, "[vpstats] prog %016llX: %u slots, attrs %04X: %.1f draws, %.0f indices, %.0f unique vertices per frame\n",
                    (unsigned long long)s_vps[j].h, s_vps[j].slots, s_vps[j].attrs, (double)s_vps[j].draws / s_sec_frames,
                    (double)s_vps[j].idx / s_sec_frames, (double)s_vps[j].uniq / s_sec_frames);
        }
        fprintf(stderr, "[vpstats] %u distinct programs\n", s_nvps);
        s_nvps = 0;
    }
    {
        double f = s_sec_frames;
        n += (size_t)snprintf(out + n, cap - n, " [repeat: vinput %.0f vbind %.0f runs hit %.0f miss %.0f copy %.0f KB conv %.0f KB;"
                              " pipe binds %.0f; vs blocks %.0f fs blocks %.0f ubo binds %.0f; image pushes %.0f; stage cache hits %.0f]",
                              s_rep[0] / f, s_rep[1] / f, s_rep[2] / f, s_rep[3] / f, s_rep[4] / f / 1024, s_rep[5] / f / 1024,
                              s_rep[6] / f, s_rep[7] / f, s_rep[8] / f, s_rep[9] / f, s_rep[10] / f, s_rep[11] / f);
        memset(s_rep, 0, sizeof s_rep);
    }
    if (s_merged) {
        n += (size_t)snprintf(out + n, cap - n, " [merged draws %.0f: regs %.2f runs %.2f copy %.2f ms (%.0f KB)]",
                              (double)s_merged / s_sec_frames, s_jn[1] / 1e6 / s_sec_frames, s_jn[2] / 1e6 / s_sec_frames,
                              s_jn[3] / 1e6 / s_sec_frames, s_jn[4] / 1024.0 / s_sec_frames);
        memset(s_jn, 0, sizeof s_jn);
        {
            int r;
            fprintf(stderr, "[mergerej] base refused: runs>1 %.0f conv %.0f cdirect %.0f notall %.0f stride %.0f topo %.0f;",
                    s_srej[1] / (double)s_sec_frames, s_srej[2] / (double)s_sec_frames, s_srej[3] / (double)s_sec_frames,
                    s_srej[4] / (double)s_sec_frames, s_srej[5] / (double)s_sec_frames, s_srej[7] / (double)s_sec_frames);
            memset(s_srej, 0, sizeof s_srej);
            fprintf(stderr, " no pending %.0f;", s_jnopend / (double)s_sec_frames);
            for (r = 1; r < 16; r++) if (s_jrej[r]) fprintf(stderr, " r%d %.0f", r, s_jrej[r] / (double)s_sec_frames);
            fprintf(stderr, "\n");
            memset(s_jrej, 0, sizeof s_jrej);
            s_jnopend = 0;
        }
        s_merged = 0;
    }
    if (s_ms_n) {
        fprintf(stderr, "[mergestats] %.0f draws/frame: regs same as previous %.0f; same state+window %.0f "
                "(vertex constants too: %.0f, constants differ: %.0f)\n", s_ms_n / (double)s_sec_frames,
                s_ms_regs / (double)s_sec_frames, s_ms_k / (double)s_sec_frames,
                s_ms_all / (double)s_sec_frames, s_ms_noconst / (double)s_sec_frames);
        s_ms_n = s_ms_all = s_ms_noconst = s_ms_regs = s_ms_k = 0;
    }
    s_nverts = s_nidx = s_nspan = 0;
    s_passes = s_passes_ms = s_uploads = 0;
    for (i = 0; i < SEC_N && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, " %s %.2f", s_sec_name[i], s_sec_ns[i] / 1e6 / s_sec_frames);
    memset(s_sec_ns, 0, sizeof s_sec_ns);
    s_sec_frames = 0;
    return 1;
}

/* RECOMP_VK_MERGESTATS=1 (measurement): how many draws could join the
 * previous one -- the same state in every respect except their vertex
 * window into the same arrays (all direct attributes moved by the same
 * whole number of vertices). Printed with the CPU stats. */

static void merge_stats(const Nv2aRawBatch *b)
{
    static int on = -1;
    static uint32_t pregs[0x2000 / 4];
    static float pconsts[192][4];
    static Nv2aRawBatch pb;
    static int have;
    int same_regs, same_consts, same_misc, same_k = 1;
    long long k = 0;
    uint32_t a;
    if (on < 0) { const char *e = getenv("RECOMP_VK_MERGESTATS"); on = e && *e == '1'; }
    if (!on)
        return;
    s_ms_n++;
    if (have) {
        uint32_t i;
        same_regs = 1;
        for (i = 0; i < 0x2000 / 4 && same_regs; i++) {
            uint32_t m = i * 4;
            if (m == 0x17FC || (m >= 0x1800 && m < 0x1820))
                continue;                       /* begin/end, index and inline data */
            if (b->regs[i] != pregs[i]) same_regs = 0;
        }
        same_consts = b->vp_const_gen == pb.vp_const_gen ||
                      (b->vp_consts && !memcmp(b->vp_consts, pconsts, sizeof pconsts));
        same_misc = b->prim == pb.prim && b->xform == pb.xform && b->surface.color_va == pb.surface.color_va
                 && b->zeta_va == pb.zeta_va && b->vp_prog_gen == pb.vp_prog_gen && b->vp_start == pb.vp_start
                 && !memcmp(b->tex_va, pb.tex_va, sizeof b->tex_va) && !memcmp(b->pal_va, pb.pal_va, sizeof b->pal_va)
                 && b->attr_present == pb.attr_present && b->attr_direct == pb.attr_direct
                 && b->attr_present == b->attr_direct
                 && b->attr_const_gen == pb.attr_const_gen
                 && !memcmp(b->composite, pb.composite, sizeof b->composite)
                 && !memcmp(b->vp_offset, pb.vp_offset, sizeof b->vp_offset);
        for (a = 0; a < NV2A_RAW_ATTRS && same_misc; a++) {
            long long d, kk;
            if (!(b->attr_direct & (1u << a)))
                continue;
            if (b->direct[a].type != pb.direct[a].type || b->direct[a].size != pb.direct[a].size
                || b->direct[a].stride != pb.direct[a].stride || !b->direct[a].stride) { same_k = 0; break; }
            d = (long long)(b->direct[a].ptr - pb.direct[a].ptr);
            if (d % (long long)b->direct[a].stride) { same_k = 0; break; }
            kk = d / (long long)b->direct[a].stride;
            if (a == (uint32_t)__builtin_ctz(b->attr_direct)) k = kk;
            else if (kk != k) { same_k = 0; break; }
        }
        if (same_regs) s_ms_regs++;
        if (same_regs && same_misc && same_k) {
            s_ms_k++;
            if (same_consts) s_ms_all++;
            else s_ms_noconst++;
        }
    }
    memcpy(pregs, b->regs, sizeof pregs);
    if (b->vp_consts) memcpy(pconsts, b->vp_consts, sizeof pconsts);
    pb = *b;
    have = 1;
}

/* RECOMP_VK_MERGE (default 1): a draw whose whole state equals the previous
 * draw's -- registers (all but the index/inline data), program, constants,
 * textures, target, primitive, and the same interleaved vertex layout --
 * joins it: its vertices go into the ring a whole number of vertices past
 * the previous draw's binding, its indices follow (strips stitched with
 * degenerate triangles) and one vkCmdDrawIndexed draws both. Conker draws a
 * model as many index ranges of one buffer: 62% of the heavy barn frame's
 * draws (872 of 1413), each ~13 us of translation and driver work.
 * Primitive order is kept, so the image is the same. The draw itself waits
 * in s_pd until something else needs the command buffer (pd_flush). */
static struct {
    int        active, strip, masked;
    uint32_t   n, cb_gen, last;     /* last: the final index (the ring is write-combined: no reads) */
    VkDeviceSize ibase;             /* its indices in the index ring */
    VkSurf    *rt;
    uint32_t   nrun;                /* the first draw's runs (s_um) */
    VkDeviceSize at[NV2A_RAW_ATTRS];
    uint32_t   stride[NV2A_RAW_ATTRS];
    int        ofr[NV2A_RAW_ATTRS];
    uint32_t   off[NV2A_RAW_ATTRS];
    uint32_t   regs[0x2000 / 4];
    Nv2aRawBatch b;                 /* the first draw's batch (pointers stale) */
} s_pend;

static int pd_room(uint32_t n)
{
    return s_pend.ibase + (VkDeviceSize)(s_pend.n + n + 4) * 4 <= IRING_BYTES;
}

static void pd_flush(void)
{
    if (!s_pend.active)
        return;
    s_pend.active = 0;
    if (!s_pend.n || s_pend.cb_gen != s_cb_gen || !s_in_render)
        return;
    vkCmdBindIndexBuffer(s_cb, s_f->iring, s_pend.ibase, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(s_cb, s_pend.n, 1, 0, 0, 0);
}

static int merge_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VK_MERGE");
        on = !(e && *e == '0') && !getenv("RECOMP_GL_PROBE");
    }
    return on;
}

/* After a draw's state is bound: wait instead of drawing, if it can be a
 * merge base. Returns 0 when the caller should draw now. */
static int pd_start(const Nv2aRawBatch *b, const uint32_t *idx, uint32_t n, VkPrimitiveTopology topo)
{
    uint32_t i;
    if (!merge_on() || s_exp) return 0;
    if (!s_um.ok) { s_srej[s_um.why]++; return 0; }
    if (topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        { s_srej[7]++; return 0; }
    s_pend.n = 0;
    s_pend.ibase = s_f->ioff;
    if (!pd_room(n))
        return 0;
    memcpy(s_f->iring_ptr + s_pend.ibase, idx, (size_t)n * 4);
    s_pend.n = n;
    s_pend.last = idx[n - 1];
    s_f->ioff = s_pend.ibase + (VkDeviceSize)n * 4;
    (void)i;
    s_pend.strip = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    s_pend.cb_gen = s_cb_gen;
    s_pend.rt = s_rt;
    s_pend.nrun = s_um.nrun;
    memcpy(s_pend.at, s_um.at, sizeof s_pend.at);
    memcpy(s_pend.stride, s_um.stride, sizeof s_pend.stride);
    memcpy(s_pend.ofr, s_um.ofr, sizeof s_pend.ofr);
    memcpy(s_pend.off, s_um.off, sizeof s_pend.off);
    s_pend.masked = nv2a_raw_dirty != NULL;
    if (s_pend.masked) {                /* only the blocks pd_join compares by hand */
        memcpy(s_pend.regs + 0x17C0 / 4, b->regs + 0x17C0 / 4, 0x80);
    } else {
        memcpy(s_pend.regs, b->regs, sizeof s_pend.regs);
    }
    s_pend.b = *b;
    s_pend.active = 1;
    return 1;
}

/* Joins b to the pending draw when everything but its vertex window matches. */
static int pd_join(const Nv2aRawBatch *b, const uint32_t *idx, uint32_t n, VkPrimitiveTopology topo)
{
    const Nv2aRawBatch *a = &s_pend.b;
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t i, k, extra, len, nrun;
    VkDeviceSize at;
    if (!s_pend.active || !n || s_pend.cb_gen != s_cb_gen || !s_in_render || s_rt != s_pend.rt
        || (topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) != s_pend.strip
        || (topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP))
        { s_jrej[1]++; return 0; }
    if (b->prim != a->prim || b->xform != a->xform || b->attr_present != a->attr_present
        || b->attr_direct != a->attr_direct || b->attr_present != b->attr_direct
        || b->vp_prog_gen != a->vp_prog_gen || b->vp_start != a->vp_start
        || b->vp_const_gen != a->vp_const_gen || (b->xform == 2 && !b->vp_const_gen)
        || b->attr_const_gen != a->attr_const_gen || b->zeta_va != a->zeta_va
        || memcmp(&b->surface, &a->surface, sizeof b->surface)
        || memcmp(b->tex_va, a->tex_va, sizeof b->tex_va) || memcmp(b->pal_va, a->pal_va, sizeof b->pal_va)
        || memcmp(b->composite, a->composite, sizeof b->composite)
        || memcmp(b->vp_offset, a->vp_offset, sizeof b->vp_offset)
        || b->aa_sx != a->aa_sx || b->aa_sy != a->aa_sy)
        { s_jrej[2]++; return 0; }
    for (i = 0; i < NV2A_RAW_ATTRS; i++)
        if ((b->attr_direct & (1u << i))
            && (b->direct[i].type != a->direct[i].type || b->direct[i].size != a->direct[i].size
                || b->direct[i].stride != a->direct[i].stride))
            { s_jrej[3]++; return 0; }
    uint64_t jt0 = hz_now(), jt1;
    /* the registers, but for SET_BEGIN_END and the index/inline data: the
     * caller's change mask when there is one (blocks 0x17C0 and 0x1800 hold
     * those and are compared by hand), else all 8 KB */
    if (nv2a_raw_dirty && s_pend.masked) {
        if (nv2a_raw_dirty[0] | (nv2a_raw_dirty[1] & ~((1ull << (0x17C0 / 64 - 64)) | (1ull << (0x1800 / 64 - 64))))
            || memcmp(b->regs + 0x17C0 / 4, s_pend.regs + 0x17C0 / 4, 0x3C)
            || memcmp(b->regs + 0x1820 / 4, s_pend.regs + 0x1820 / 4, 0x20))
            { s_jrej[4]++; return 0; }
    } else if (!s_pend.masked && (memcmp(b->regs, s_pend.regs, 0x17FC)
               || memcmp(b->regs + 0x1820 / 4, s_pend.regs + 0x1820 / 4, 0x2000 - 0x1820))) {
        { s_jrej[5]++; return 0; }
    } else if (s_pend.masked && !nv2a_raw_dirty) {
        { s_jrej[6]++; return 0; }                       /* no full copy to compare with */
    }
    jt1 = hz_now(); s_jn[1] += jt1 - jt0; jt0 = jt1;
    nrun = vertex_runs(b, b->attr_direct, run, of_run);
    if (nrun != s_pend.nrun)
        { s_jrej[7]++; return 0; }
    {   /* the same run structure: map[j] = the first draw's run for b's run j */
        int map[NV2A_RAW_ATTRS];
        long dlt[NV2A_RAW_ATTRS];       /* run j sits dlt[j] bytes past the slot of its first byte */
        VkDeviceSize pos[NV2A_RAW_ATTRS], end = 0;
        uint32_t j, tries;
        for (j = 0; j < nrun; j++) map[j] = -1;
        for (i = 0; i < NV2A_RAW_ATTRS; i++) {
            long d;
            if (!(b->attr_direct & (1u << i)))
                continue;
            j = (uint32_t)of_run[i];
            /* run starts are rounded down to 4 bytes, so an attribute's offset
             * in its run can differ by a few bytes between draws (stride 6) */
            d = (long)s_pend.off[i] - (long)(b->direct[i].ptr - run[j].lo);
            if ((map[j] >= 0 && (map[j] != s_pend.ofr[i] || dlt[j] != d)))
                { s_jrej[10]++; return 0; }
            map[j] = s_pend.ofr[i];
            dlt[j] = d;
        }
        /* vertex v of the merged draw sits at at[r] + v * stride[r] in every
         * run r: one shift k for all runs, the smallest that puts each window
         * past the ring's end without two of them overlapping */
        k = 0;
        for (j = 0; j < nrun; j++) {
            uint32_t r = (uint32_t)map[j], st = s_pend.stride[r];
            VkDeviceSize b0 = s_pend.at[r] + (VkDeviceSize)(dlt[j] + 8) - 8;
            if (s_f->ring_off > b0) {
                uint32_t kk = (uint32_t)((s_f->ring_off - b0 + st - 1) / st);
                if (kk > k) k = kk;
            }
        }
        {   /* the smallest k >= k0 with no two windows overlapping: the gap
             * between runs j and q changes by (stride q - stride j) a step, so
             * the candidates are k0 and each pair's first clear k */
            uint32_t cand[1 + 2 * NV2A_RAW_ATTRS * NV2A_RAW_ATTRS], nc = 0, c, q, best = ~0u;
            cand[nc++] = k;
            for (j = 0; j < nrun; j++)
                for (q = 0; q < nrun; q++) {
                    long long sj = s_pend.stride[map[j]], sq = s_pend.stride[map[q]];
                    long long aj = (long long)s_pend.at[map[j]] + dlt[j], aq = (long long)s_pend.at[map[q]] + dlt[q];
                    long long lj = (long long)(run[j].hi - run[j].lo), dS = sq - sj, dA = aq - aj, kk;
                    if (j == q || dS <= 0)
                        continue;
                    kk = (lj - dA + dS - 1) / dS;           /* q starts past j's end */
                    if (lj - dA <= 0) kk = 0;
                    if (kk > (long long)k && kk < 0x40000000LL && nc < sizeof cand / sizeof cand[0])
                        cand[nc++] = (uint32_t)kk;
                }
            for (tries = 0; tries < nc; tries++) {
                uint32_t ok = 1;
                c = cand[tries];
                if (c >= best)
                    continue;
                for (j = 0; j < nrun; j++)
                    pos[j] = s_pend.at[map[j]] + (VkDeviceSize)(dlt[j] + 8) - 8 + (VkDeviceSize)c * s_pend.stride[map[j]];
                for (j = 0; j < nrun && ok; j++)
                    for (q = j + 1; q < nrun && ok; q++)
                        if (pos[j] < pos[q] + (VkDeviceSize)(run[q].hi - run[q].lo)
                            && pos[q] < pos[j] + (VkDeviceSize)(run[j].hi - run[j].lo))
                            ok = 0;
                if (ok)
                    best = c;
            }
            if (best == ~0u)
                { s_jrej[11]++; return 0; }
            k = best;
            end = 0;
            for (j = 0; j < nrun; j++) {
                pos[j] = s_pend.at[map[j]] + (VkDeviceSize)(dlt[j] + 8) - 8 + (VkDeviceSize)k * s_pend.stride[map[j]];
                if (pos[j] + (VkDeviceSize)(run[j].hi - run[j].lo) > end)
                    end = pos[j] + (VkDeviceSize)(run[j].hi - run[j].lo);
            }
        }
        if (end + 64 + (VkDeviceSize)(s_pend.n + n + 4) * 4 > RING_BYTES)
            { s_jrej[8]++; return 0; }                   /* the flush must not need a ring flush */
        {   /* streams of different strides drift apart as k grows: the gaps
             * are wasted ring (a 48 MB ring filled mid-frame and flushed --
             * 9.8 ms -- in barn_cmp); over twice the data plus 4 KB, start a
             * new draw instead */
            VkDeviceSize data = 0;
            for (j = 0; j < nrun; j++)
                data += (VkDeviceSize)(run[j].hi - run[j].lo);
            if (end - s_f->ring_off > 2 * data + 4096)
                { s_jrej[12]++; return 0; }
        }
        if (!pd_room(n + 3))
            { s_jrej[9]++; return 0; }
        jt1 = hz_now(); s_jn[2] += jt1 - jt0; jt0 = jt1;
        for (j = 0; j < nrun; j++) {
            len = (uint32_t)(run[j].hi - run[j].lo);
            memcpy(s_f->ring_ptr + pos[j], run[j].lo, len);
            s_jn[4] += len;
        }
        s_f->ring_off = (end + 15) & ~(VkDeviceSize)15;
        jt1 = hz_now(); s_jn[3] += jt1 - jt0;
    }
    {
        uint32_t *o = (uint32_t *)(s_f->iring_ptr + s_pend.ibase), m = s_pend.n;
        if (s_pend.strip) {               /* degenerate stitch, B on an even position */
            uint32_t last = s_pend.last, first = idx[0] + k;
            o[m++] = last;
            o[m++] = first;
            if (m & 1)
                o[m++] = first;
        }
        for (i = 0; i < n; i++)
            o[m + i] = idx[i] + k;
        s_pend.n = m + n;
        s_pend.last = idx[n - 1] + k;
        s_f->ioff = s_pend.ibase + (VkDeviceSize)s_pend.n * 4;
        (void)extra;
    }
    s_merged++;
    return 1;
}

static void vk_draw_raw(const Nv2aRawBatch *b)
{
    VkSurf *s, *sampled[4];
    VkDepthBuf *d;
    VkProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, i, cls;
    VkPrimitiveTopology topo;
    VkDescriptorImageInfo img[4];
    float tscale[4][2];
    PipeKey key;
    VkPipeline pipe;
    VkDescriptorBufferInfo ub[2];
    VkWriteDescriptorSet wr[6];
    void *map;
    static VkPipeline cur_pipe;
    static uint32_t cur_pipe_gen;

    if (s_state <= 0 && !ready())   /* ready() itself costs ~1 ms a call on the console (big frame) */
        return;
    s_regs = r;
    s_batch = b;
    merge_stats(b);
    s_want_ss = 0;
    ft_start(s_frame);
    s_ft_draws[s_frame % FT_N]++;
    s_sec_t = hz_now();
    if (!prim_indices(b, &idx, &n, &topo, &cls) || !n)
        return;
    if (!s_pend.active) s_jnopend++;
    if (s_pend.active) {
        if (pd_join(b, idx, n, topo)) {
            if (s_pp_cur) { s_pp_cur->draws++; s_pp_cur->tris += n / 3; }
            s_nverts += b->vertex_count;
            s_nidx += n;
            SEC(SEC_VERT);
            return;
        }
        pd_flush();
    }
    VT("draw: prim %u idx %u verts %u attrs %04X direct %04X xform %u surf %08X zeta %08X\n", b->prim, n,
       b->vertex_count, b->attr_present, b->attr_direct, b->xform, b->surface.color_va, b->zeta_va);
    s_target_regs = r;
    s = target(&b->surface, b->zeta_va, &d);
    s_target_regs = NULL;
    if (!s)
        return;
    s_drew_any = 1;
    {   /* RECOMP_VK_POSTLOG=1 (diagnostic): draws into a surface after its
         * mid-frame resolve -- what the flip resolve has to redo */
        static int pl = -1;
        if (pl < 0) { const char *e = getenv("RECOMP_VK_POSTLOG"); pl = e && *e == '1'; }
        if (pl && s->va == s_post_va && s_frame < 4)
            fprintf(stderr, "[postlog] f%u into %08X: prim %u idx %u verts %u xform %u blend %u zen %u\n",
                    s_frame, s->va, b->prim, n, b->vertex_count, b->xform, r[0x304 / 4], r[0x30C / 4], r[0x35C / 4], r[0x32C / 4]);
    }
    psh_key(r, &pk);
    {   /* Cube-map stages, once per (texture modes, transform) combination:
         * a vertex program (xform 2) writes the lookup vector itself; the
         * fixed-function path would need texgen (SET_TEXGEN_S..Q 0x03C0+16i),
         * which gl_vsh.c does not do yet. */
        static uint32_t seen[16];
        static int nseen;
        uint32_t sp = pk.shader_program, k2, t;
        int cube = 0;
        for (t = 0; t < 4; t++)
            cube |= nv2a_psh_stage_is_cube(&pk, (int)t);
        for (k2 = 0; k2 < (uint32_t)nseen && seen[k2] != (sp ^ (b->xform << 28)); k2++) { }
        if (cube && k2 == (uint32_t)nseen && nseen < 16) {
            seen[nseen++] = sp ^ (b->xform << 28);
            LOGE("cube draw: modes %05X xform %u vp_slots %u attrs %04X fmt %08X %08X"
                 " texgen0 %X %X %X %X texgen1 %X %X %X %X\n",
                 sp, b->xform, b->vp_slots, b->attr_present, r[0x1B04 / 4], r[0x1B44 / 4],
                 r[0x03C0 / 4], r[0x03C4 / 4], r[0x03C8 / 4], r[0x03CC / 4],
                 r[0x03D0 / 4], r[0x03D4 / 4], r[0x03D8 / 4], r[0x03DC / 4]);
        }
    }
    {
        uint32_t made = s_prog_made, a;
        uint64_t t0 = hz_now();
        s_cmp = 0;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if ((b->attr_direct & (1u << a)) && b->direct[a].type == 6
                && vertex_format(6, b->direct[a].size) != VK_FORMAT_UNDEFINED)
                s_cmp |= 1u << a;
        p = prog_get(b, &pk);
        if (s_prog_made != made) {
            s_hz_progs += s_prog_made - made;
            s_hz_prog_ns += hz_now() - t0;
        }
    }
    if (!p)
        return;
    s_hz_draws++;
    SEC(SEC_PROG);

    /* Textures first: uploads end the rendering pass. */
    memset(img, 0, sizeof img);     /* padding: compared whole (descriptor reuse) */
    for (i = 0; i < 4; i++)
        bind_stage(r, (int)i, tscale[i], &img[i], &sampled[i]);
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i]->dirty && sampled[i] != s) {
            end_rendering();                 /* next begin_rendering barriers */
            break;
        }

    memset(&key, 0, sizeof key);
    key.prog = (int)(p - s_prog);
    if (r[0x304 / 4]) {
        key.blend = 1; key.bsrc = r[0x344 / 4]; key.bdst = r[0x348 / 4]; key.beq = r[0x350 / 4];
    }
    {
        uint32_t cm = r[0x358 / 4], m = 0;
        if (cm & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (cm & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (cm & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
        if (cm & 0x01000000u) m |= VK_COLOR_COMPONENT_A_BIT;
        key.cmask = m;
    }
    {   /* ss_post: no depth test, depth write or stencil test */
        static int on = -1;
        if (on < 0) { const char *e = getenv("RECOMP_VK_SS_POST"); on = e && *e == '1'; }
        if (on && s->ms_view && !s->ms_dirty && !r[0x30C / 4] && !r[0x35C / 4] && !r[0x32C / 4]) {
            s_want_ss = 1;
            d = NULL;
        }
    }
    key.depth = d != NULL;
    key.topo = cls;
    key.samples = s->ms_view && !s_want_ss ? (uint32_t)s_msaa : 1u;
    SEC(SEC_TEX);
    pipe = pipe_get(&key);
    if (!pipe)
        return;
    SEC(SEC_PIPE);

    {
        /* Everything this draw puts in the ring, reserved now: a flush
         * later in the draw would reuse the ring under its own uniforms. */
        VkDeviceSize need = 2 * 4096 + sizeof(VsBlock) + sizeof(FsBlock) + (VkDeviceSize)n * 4
                          + NV2A_RAW_ATTRS * 16 + 16 * 16;
        uint32_t a;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (b->attr_present & (1u << a)) {
                uint32_t st = (b->attr_direct & (1u << a)) ? b->direct[a].stride : 0;
                need += (VkDeviceSize)b->vertex_count * (st > 16 ? st : 16) + 16;
            }
        if (s_f->ring_off + need > RING_BYTES) {
            void *tmp;
            if (need > RING_BYTES) return;
            ring_alloc(RING_BYTES, 16, &tmp);       /* forces the flush */
            s_f->ring_off = 0;
        }
    }
    begin_rendering(s, d);
    s->dirty = 1;
    if (s_exp < 0) { const char *e = getenv("RECOMP_VK_EXP"); s_exp = e ? atoi(e) : 0; }
    if ((s_exp & 2) && cur_pipe_gen == s_cb_gen) pipe = cur_pipe;
    if (pipe != cur_pipe || cur_pipe_gen != s_cb_gen) {
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        s_rep[6]++;
        cur_pipe = pipe;
        cur_pipe_gen = s_cb_gen;
    }
    {
        static uint32_t dyn_pass = ~0u;
        if (!(s_exp & 16) || dyn_pass != s_passes) apply_dynamic(r, d != NULL, topo);
        dyn_pass = s_passes;
    }
    set_surface_scissor(r, s);
    SEC(SEC_PASS);

    /* Uniforms. */
    {
        VsBlock *vb;
        FsBlock *fb;
        /* The vertex block is 3 KB (192 constants): reuse this frame's last one
         * when nothing in it changed -- vp_const_gen equal means identical
         * constants -- instead of copying it for every draw. */
        struct VsKey {
            uint32_t gen, cb_gen, zmax, xform;
            float sw, sh, m[16], vpoff[4], aa[2];
        } vk;
        static struct VsKey last_vk;
        static VkDeviceSize last_vat;
        static int last_vk_ok;
        memset(&vk, 0, sizeof vk);
        vk.gen = b->vp_const_gen;
        vk.cb_gen = s_cb_gen;
        vk.zmax = zmax_of(r);
        vk.xform = b->xform == 1 ? 1 : 0;
        vk.sw = (float)s->w;
        vk.sh = (float)s->h;
        memcpy(vk.m, b->composite, sizeof vk.m);
        memcpy(vk.vpoff, b->vp_offset, sizeof vk.vpoff);
        vk.aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
        vk.aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
        if (last_vk_ok && b->vp_const_gen && !memcmp(&vk, &last_vk, sizeof vk)) {
            ub[0].offset = last_vat;
        } else {
            VkDeviceSize vat = ring_alloc(sizeof(VsBlock), s_ubo_align, &map);
            s_rep[7]++;
            vb = (VsBlock *)map;
            if (!vb) return;
            memcpy(vb->c, b->vp_consts, sizeof vb->c);
            vb->surf[0] = 2.0f / vk.sw;
            vb->surf[1] = 2.0f / vk.sh;
            vb->surf[2] = 1.0f / (float)vk.zmax;
            vb->surf[3] = 0.0f;
            memcpy(vb->m, vk.m, sizeof vb->m);
            memcpy(vb->vpoff, vk.vpoff, sizeof vb->vpoff);
            vb->aa[0] = vk.aa[0];
            vb->aa[1] = vk.aa[1];
            vb->xform = (int32_t)vk.xform;
            ub[0].offset = vat;
            last_vk = vk;
            last_vat = vat;
            last_vk_ok = s_cb_gen == vk.cb_gen;
            last_vk.cb_gen = s_cb_gen;       /* a mid-frame flush restarts the ring */
        }
        ub[0].buffer = s_f->ring;
        ub[0].range = sizeof(VsBlock);

        {
            static FsBlock fbl, last_fb;
            static VkDeviceSize last_fat;
            static uint32_t last_fb_gen;
            static int last_fb_ok;
            fb = &fbl;
            memset(fb, 0, sizeof *fb);
            for (i = 0; i < 4; i++) {
                fb->tscale[i][0] = tscale[i][0];
                fb->tscale[i][1] = tscale[i][1];
            }
            for (i = 0; i < 8; i++) {
                argb_vec4(r[0xA60 / 4 + i], fb->c0[i]);
                argb_vec4(r[0xA80 / 4 + i], fb->c1[i]);
            }
            argb_vec4(r[0x1E20 / 4], fb->fc0);
            argb_vec4(r[0x1E24 / 4], fb->fc1);
            {
                uint32_t fc = r[0x2A8 / 4];
                fb->fog[0] = byte_f(fc, 0); fb->fog[1] = byte_f(fc, 8);
                fb->fog[2] = byte_f(fc, 16); fb->fog[3] = byte_f(fc, 24);
            }
            fb->afunc = r[0x300 / 4] ? (int32_t)r[0x33C / 4] : 0x207;
            fb->aref = (float)(r[0x340 / 4] & 0xFF);
            for (i = 0; i < 4; i++) {
                /* SET_TEXTURE_SET_BUMP_ENV_MAT (0x1B28..), _SCALE 0x1B38, _OFFSET 0x1B3C */
                memcpy(fb->bump[i], &r[(0x1B28u + (uint32_t)i * 0x40u) / 4], 16);
                memcpy(fb->bumpl[i], &r[(0x1B38u + (uint32_t)i * 0x40u) / 4], 8);
            }
            window_clip_block(fb, r, s);
            if (last_fb_ok && last_fb_gen == s_cb_gen && !memcmp(&fbl, &last_fb, sizeof fbl)) {
                ub[1].offset = last_fat;
            } else {
                VkDeviceSize fat = ring_alloc(sizeof(FsBlock), s_ubo_align, &map);
                s_rep[8]++;
                if (!map) return;
                memcpy(map, &fbl, sizeof fbl);
                last_fb = fbl;
                last_fat = fat;
                last_fb_gen = s_cb_gen;          /* after ring_alloc: a flush bumps it */
                last_fb_ok = 1;
                ub[1].offset = fat;
            }
            ub[1].buffer = s_f->ring;
            ub[1].range = sizeof(FsBlock);
        }
    }
    SEC(SEC_UNIF);
    memset(wr, 0, sizeof wr);
    for (i = 0; i < 6; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i < 2) {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &ub[i];
        } else {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &img[i - 2];
        }
    }
    if (s_dynubo) {
        static uint32_t last_off[2], last_gen, last_fi = ~0u;
        uint32_t off[2] = { (uint32_t)ub[0].offset, (uint32_t)ub[1].offset };
        if (last_gen != s_cb_gen || last_fi != (uint32_t)s_fi || memcmp(off, last_off, sizeof off)) {
            vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1,
                                    &s_ubo_set[s_fi], 2, off);
            s_rep[9]++;
            memcpy(last_off, off, sizeof off);
            last_gen = s_cb_gen;
            last_fi = (uint32_t)s_fi;
        }
        {   /* the same four images as the last draw in this command buffer */
            static VkDescriptorImageInfo last_img[4];
            static uint32_t last_img_gen;
            if (last_img_gen != s_cb_gen || memcmp(img, last_img, sizeof last_img)) {
                p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 4, wr + 2);
                memcpy(last_img, img, sizeof last_img);
                last_img_gen = s_cb_gen;
                s_rep[10]++;
            }
        }
    } else {
        static uint32_t desc_pass = ~0u;
        if (!(s_exp & 1) || desc_pass != s_passes)
            p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 6, wr);
        desc_pass = s_passes;
    }

    SEC(SEC_DESC);
    if (!upload_vertices(b))
        return;
    if (pd_start(b, idx, n, topo)) {
        SEC(SEC_VERT);
    } else if (s_f->ioff + (VkDeviceSize)n * 4 <= IRING_BYTES) {
        memcpy(s_f->iring_ptr + s_f->ioff, idx, (size_t)n * 4);
        vkCmdBindIndexBuffer(s_cb, s_f->iring, s_f->ioff, VK_INDEX_TYPE_UINT32);
        s_f->ioff += (VkDeviceSize)n * 4;
        SEC(SEC_VERT);
        VT("  vkCmdDrawIndexed %u\n", n);
        if (!(s_exp & 8))
            vkCmdDrawIndexed(s_cb, (s_exp & 32) && n > 3 ? 3 : n, 1, 0, 0, 0);
    } else {
        VkDeviceSize iat;
        if (b->vertex_count <= 65536) {
            uint16_t *o;
            iat = ring_alloc((VkDeviceSize)n * 2, 4, &map);
            o = (uint16_t *)map;
            if (!o) return;
            for (i = 0; i < n; i++)
                o[i] = (uint16_t)idx[i];
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT16);
        } else {
            iat = ring_alloc((VkDeviceSize)n * 4, 4, &map);
            if (!map) return;
            memcpy(map, idx, (size_t)n * 4);
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT32);
        }
        SEC(SEC_VERT);
        VT("  vkCmdDrawIndexed %u\n", n);
        if (!(s_exp & 8))
            vkCmdDrawIndexed(s_cb, (s_exp & 32) && n > 3 ? 3 : n, 1, 0, 0, 0);   /* 32: one triangle a draw */
    }
    if (s_pp_cur) { s_pp_cur->draws++; s_pp_cur->tris += n / 3; }
    if (s_vps_on < 0) { const char *e = getenv("RECOMP_VK_VPSTATS"); s_vps_on = e && *e == '1'; }
    s_nverts += b->vertex_count;
    s_nidx += n;
    s_nprim[b->prim & 15] += n;
    if (s_vps_on) {
        uint32_t lo = ~0u, hi = 0, q;
        for (q = 0; q < n; q++) { if (idx[q] < lo) lo = idx[q]; if (idx[q] > hi) hi = idx[q]; }
        if (n) s_nspan += hi - lo + 1;
        if (n && hi - lo < (1u << 20)) {
            static uint8_t *seen; static uint32_t seen_cap;
            uint32_t span = hi - lo + 1;
            if (span > seen_cap) { free(seen); seen = (uint8_t *)malloc(span); seen_cap = span; }
            if (seen) {
                memset(seen, 0, span);
                s_last_uniq = 0;
                for (q = 0; q < n; q++) if (!seen[idx[q] - lo]) { seen[idx[q] - lo] = 1; s_nuniq++; s_last_uniq++; }
            }
        }
        for (q = 0; q + 2 < n; q += 3)
            if (idx[q] == idx[q + 1] || idx[q + 1] == idx[q + 2] || idx[q] == idx[q + 2]) s_ndegen++;
    }
    if (s_vps_on < 0) { const char *e = getenv("RECOMP_VK_VPSTATS"); s_vps_on = e && *e == '1'; }
    if (s_vps_on) {
        uint64_t h = 1469598103934665603ull;
        uint32_t pc, k, end = b->vp_slots;
        if (b->xform == 2 && b->vp_program) {
            for (pc = b->vp_start; pc < b->vp_slots; pc++) {
                const uint8_t *by = (const uint8_t *)b->vp_program[pc];
                for (k = 0; k < 16; k++) h = (h ^ by[k]) * 1099511628211ull;
                if (b->vp_program[pc][3] & 1u) { end = pc + 1; break; }
            }
        } else
            h = b->xform, end = 0;
        for (k = 0; k < s_nvps && s_vps[k].h != h; k++) { }
        if (k == s_nvps && k < VPS_N) { s_nvps++; s_vps[k].h = h; s_vps[k].slots = end > b->vp_start ? end - b->vp_start : 0;
                                        s_vps[k].draws = s_vps[k].idx = s_vps[k].uniq = 0; }
        if (k < VPS_N) {
            s_vps[k].attrs = b->attr_present;
            s_vps[k].draws++;
            s_vps[k].idx += n;
            s_vps[k].uniq += s_last_uniq;
        }
    }
    SEC(SEC_DRAW);
    {   /* RECOMP_GL_PROBE=<frame>: nv2a_gl.c's per-draw probe, same line, so
         * the two back ends' draw sequences diff (diagnostic, slow). */
        static int probe_init;
        static uint32_t probe_frame, probe_last, probe_n;
        if (!probe_init) {
            const char *e = getenv("RECOMP_GL_PROBE");
            probe_init = 1;
            probe_frame = e ? (uint32_t)strtoul(e, NULL, 10) : 0;
        }
        if (probe_frame && s_frame == probe_frame) {
            uint32_t st[2] = { 0, 0 };
            if (probe_last != s_frame) { probe_last = s_frame; probe_n = 0; }
            surf_readback(s, probe_count, st);
            fprintf(stderr, "[PROBE] f%u d%u surf %08X prim %u n %u xform %u t0 %08X"
                    " blend %u ztest %u zfunc %X cmask %08X modes %05X -> nonblack %u max %u\n",
                    s_frame, probe_n++, s->va, b->prim, b->index_count, b->xform,
                    b->tex_va[0], r[0x304 / 4], r[0x30C / 4], r[0x354 / 4], r[0x358 / 4],
                    pk.shader_program, st[0], st[1]);
        }
    }
    if (s_vtrace > 0)
        s_vtrace--;
}

static void vk_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkSurf *s;
    VkDepthBuf *d;
    VkClearAttachment ca[2];
    VkClearRect rect;
    uint32_t nca = 0;

    uint64_t tc0 = hz_now(), tc1;
    pd_flush();
    if (s_state <= 0 && !ready())   /* ready() itself costs ~1 ms a call on the console (big frame) */
        return;
    s_clr_ns[3] += hz_now() - tc0;
    s = target(sf, rs ? rs->zeta_va : 0, &d);
    tc1 = hz_now();
    s_clr_ns[0] += tc1 - tc0;
    if (!s)
        return;
    memset(&rect, 0, sizeof rect);
    rect.layerCount = 1;
    rect.rect.extent.width = s->pw;
    rect.rect.extent.height = s->ph;
    static int replay = -1, full_clear = -1, clear_log_frame = -2;
    if (replay < 0) replay = getenv("RECOMP_REPLAY") != NULL;
    if (full_clear < 0) full_clear = getenv("RECOMP_VK_FULL_CLEAR") != NULL;
    if (clear_log_frame == -2) {
        const char *e = getenv("RECOMP_VK_CLEARLOG");
        clear_log_frame = e ? atoi(e) : -1;
    }
    /* SET_CLEAR_RECT from the live register file. A replay's s_regs is the
     * previous draw's record -- fixtures do not store the clear rect -- and a
     * stale 128x128 rect from the light targets' clears left most of the frame's
     * depth uncleared (alternate frames of fixtures/shadowflk.fixz went dark):
     * replayed clears cover the surface clip. */
    if (s_regs && !replay && !full_clear) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            uint32_t px0 = to_stored(x0, s->pw, s->w), py0 = to_stored(y0, s->ph, s->h);
            rect.rect.offset.x = (int32_t)px0;
            rect.rect.offset.y = (int32_t)py0;
            rect.rect.extent.width = to_stored(x1, s->pw, s->w) - px0;
            rect.rect.extent.height = to_stored(y1, s->ph, s->h) - py0;
        }
    }
    if (d && rect.rect.extent.width > d->pw - (uint32_t)rect.rect.offset.x)
        rect.rect.extent.width = d->pw - (uint32_t)rect.rect.offset.x;
    if (d && rect.rect.extent.height > d->ph - (uint32_t)rect.rect.offset.y)
        rect.rect.extent.height = d->ph - (uint32_t)rect.rect.offset.y;
    memset(ca, 0, sizeof ca);
    if ((flags & 0xF0) && sf->bytes_per_pixel == 2) {
        /* COLOR_CLEAR_VALUE is packed in the surface's own format. For a
         * 16-bit surface white is 0xFFFF, which read as A8R8G8B8 is cyan:
         * Conker clears its R5G6B5 shadow targets to white and every
         * projected shadow came out as a teal patch (nv2a_gl.c). A 16-bit
         * target has no alpha worth keeping (sampled with alpha 1), so an
         * RGB-only mask clears the whole texel. */
        uint32_t v = argb & 0xFFFF, fmt = s_regs ? (s_regs[0x208 / 4] & 0xF) : 3;
        uint32_t cr, cg, cb;
        if (fmt == 3) {                          /* R5G6B5 */
            cr = (v >> 11) & 0x1F; cg = (v >> 5) & 0x3F; cb = v & 0x1F;
            cr = (cr << 3) | (cr >> 2); cg = (cg << 2) | (cg >> 4); cb = (cb << 3) | (cb >> 2);
        } else {                                 /* X1R5G5B5 */
            cr = (v >> 10) & 0x1F; cg = (v >> 5) & 0x1F; cb = v & 0x1F;
            cr = (cr << 3) | (cr >> 2); cg = (cg << 3) | (cg >> 2); cb = (cb << 3) | (cb >> 2);
        }
        argb = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
        if ((flags & 0x70) == 0x70)
            flags |= 0x80;
    }
    if (s->ss_newer && getenv("RECOMP_VK_POSTLOG"))
        fprintf(stderr, "[postlog] clear %08X flags %X rect %d,%d %ux%u of %ux%u depth %p\n", s->va, flags,
                rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height, s->pw, s->ph, (void *)d);
    if ((flags & 0xF0) == 0xF0 && !rect.rect.offset.x && !rect.rect.offset.y
        && rect.rect.extent.width == s->pw && rect.rect.extent.height == s->ph)
        s->ss_newer = 0;            /* ss_post: the twin is overwritten anyway */
    if ((flags & 0xF0) == 0xF0) {
        ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[nca].colorAttachment = 0;
        argb_vec4(argb, ca[nca].clearValue.color.float32);
        nca++;
    } else if (flags & 0xF0) {
        static int warned;
        if (!warned++)
            LOGE("clear with a partial colour mask (0x%X) not done yet\n", flags & 0xF0);
    }
    if (d && (flags & 0x3)) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        ca[nca].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                             ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[nca].clearValue.depthStencil.depth = (float)((double)z / (double)zmax);
        ca[nca].clearValue.depthStencil.stencil = zstencil & 0xFF;
        nca++;
    }
    if (!nca)
        return;
    if ((int)s_frame == clear_log_frame)
        fprintf(stderr, "[vkclear] f%u regs %p col %08X flags %X rawz %08X zmax %08X depth %g rect %d,%d %ux%u surface %ux%u depthsize %ux%u regrect %08X/%08X\n",
                s_frame, (void *)s_regs, s->va, flags, zstencil, s_regs ? zmax_of(s_regs) : 0xFFFFFFu,
                d && (flags & 3) ? ca[nca - 1].clearValue.depthStencil.depth : -1.0,
                rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height,
                s->pw, s->ph, d ? d->pw : 0, d ? d->ph : 0,
                s_regs ? s_regs[0x1D98 / 4] : 0, s_regs ? s_regs[0x1D9C / 4] : 0);
    VT("clear %08X %ux%u flags %X rect %d,%d %ux%u depth %p\n", s->va, s->pw, s->ph, flags,
       rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height, (void *)d);
    tc0 = hz_now();
    begin_rendering(s, d);
    tc1 = hz_now();
    s_clr_ns[1] += tc1 - tc0;
    vkCmdClearAttachments(s_cb, nca, ca, 1, &rect);
    if (s_pp_cur) s_pp_cur->clears++;
    s_clr_ns[2] += hz_now() - tc1;
    VT("  cleared\n");
    s->dirty = 1;
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;
    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        static int alpha = -1;          /* RECOMP_DUMP_ALPHA=1: the alpha channel, grey */
        if (alpha < 0) alpha = getenv("RECOMP_DUMP_ALPHA") != NULL;
        for (x = 0; x < w; x++) {
            if (alpha) {
                uint8_t g[3] = { p[x * 4 + 3], p[x * 4 + 3], p[x * 4 + 3] };
                fwrite(g, 1, 3, f);
            } else {
                fwrite(p + x * 4, 1, 3, f);
            }
        }
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* RECOMP_GL_DUMP: read the surface back (waits for the GPU). */
/* Copy the surface to host memory (waits for the GPU); fn(ptr) sees BGRA rows. */
static void surf_readback(VkSurf *s, void (*fn)(const uint8_t *, VkSurf *, void *), void *ctx)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements rq;
    VkBufferImageCopy rg;
    void *ptr;

    bci.size = (VkDeviceSize)s->pw * s->ph * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mem) { vkDestroyBuffer(s_dev, buf, NULL); return; }
    vkBindBufferMemory(s_dev, buf, mem, 0);
    surf_resolve(s);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = s->pw;
    rg.imageExtent.height = s->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    flush_frame_wait();
    if (vkMapMemory(s_dev, mem, 0, bci.size, 0, &ptr) == VK_SUCCESS) {
        fn((const uint8_t *)ptr, s, ctx);
        vkUnmapMemory(s_dev, mem);
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

static void probe_count(const uint8_t *p, VkSurf *s, void *ctx)
{
    uint32_t *o = (uint32_t *)ctx, i, n = s->pw * s->ph * 4;
    o[0] = o[1] = 0;
    for (i = 0; i < n; i += 4) {
        uint32_t m = p[i] > p[i + 1] ? p[i] : p[i + 1];
        if (p[i + 2] > m) m = p[i + 2];
        if (m > 8) o[0]++;
        if (m > o[1]) o[1] = m;
    }
}

static void dump_surface(VkSurf *s, const char *path)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements rq;
    VkBufferImageCopy rg;
    void *ptr;

    bci.size = (VkDeviceSize)s->pw * s->ph * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mem) { vkDestroyBuffer(s_dev, buf, NULL); return; }
    vkBindBufferMemory(s_dev, buf, mem, 0);
    surf_resolve(s);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = s->pw;
    rg.imageExtent.height = s->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    flush_frame_wait();
    if (vkMapMemory(s_dev, mem, 0, bci.size, 0, &ptr) == VK_SUCCESS) {
        dump_bmp(path, (const uint8_t *)ptr, s->pw, s->ph);
        vkUnmapMemory(s_dev, mem);
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

/* Replay diagnostics (RECOMP_REPLAY_DUMPVA, nv2a_fixture.c): the render
 * target at va as it is now, mid-frame. Returns 0 if there is none. */
int nv2a_vk_dump_va(uint32_t va, const char *path)
{
    VkSurf *s = surf_find(va);
    if (!s)
        return 0;
    dump_surface(s, path);
    return 1;
}

/* RECOMP_VK_PRESENT_MS: draw the multisampled surface into the acquired
 * swapchain image (black bars from the clear), leaving it ready to present.
 * Same letterbox as the blit path. */
static void present_filtered(VkSurf *s, VkImage dst, VkImageView view, int multisampled)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkViewport v;
    VkRect2D full = { { 0, 0 }, s_sc_extent }, sc;
    float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
    float ww = (float)s_sc_extent.width, wh = (float)s_sc_extent.height, scale;
    struct { float off[2], scl[2]; int32_t smax[2]; float visibility; } pc;
    int dw, dh, dx, dy;

    if (xbox_video_widescreen() && lw < lh * 1.5f)
        lw = lh * 16.0f / 9.0f;
    scale = ww / lw < wh / lh ? ww / lw : wh / lh;
    dw = (int)(lw * scale); dh = (int)(lh * scale);
    dx = ((int)ww - dw) / 2; dy = ((int)wh - dh) / 2;

    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea = full;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    pp_begin(2, s, (int)s_msaa);
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, multisampled ? s_pres_pipe : s_pres_ss_pipe);
    v.x = (float)dx; v.y = (float)dy; v.width = (float)dw; v.height = (float)dh;
    v.minDepth = 0.0f; v.maxDepth = 1.0f;
    sc.offset.x = dx; sc.offset.y = dy;
    sc.extent.width = (uint32_t)dw; sc.extent.height = (uint32_t)dh;
    vkCmdSetViewport(s_cb, 0, 1, &v);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_sres_sampler;
    ii.imageView = multisampled ? s->ms_view : s->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pres_layout, 0, 1, &w);
    pc.off[0] = (float)dx; pc.off[1] = (float)dy;
    pc.scl[0] = (float)s->pw / (float)dw; pc.scl[1] = (float)s->ph / (float)dh;
    pc.smax[0] = (int32_t)s->pw - 1; pc.smax[1] = (int32_t)s->ph - 1;
    pc.visibility = xbox_platform_visibility ? xbox_platform_visibility() : 0.0f;
    vkCmdPushConstants(s_cb, s_pres_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    pp_end();
    s_cb_gen++;
    s_dyn_valid = 0;
    s_sc_cur.extent.width = 0;

    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* RECOMP_VK_FPS=1: frames per second (top) and the mean frame time in ms
 * (below) in the top-left corner of the screen, 7-segment digits drawn
 * with rectangle clears into the swapchain image after the blit; updated
 * twice a second. */
static void fps_rect(VkClearRect *r, uint32_t *n, int x, int y, int w, int h)
{
    if (*n >= 96) return;
    r[*n].rect.offset.x = x; r[*n].rect.offset.y = y;
    r[*n].rect.extent.width = (uint32_t)w; r[*n].rect.extent.height = (uint32_t)h;
    r[*n].baseArrayLayer = 0; r[*n].layerCount = 1;
    (*n)++;
}

/* "dd.d" at x, y; segment thickness t */
static int fps_number(VkClearRect *r, uint32_t *n, int x, int y, int t, double v)
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int d[4], i, w = 4 * t, h = 7 * t, iv;
    if (v < 0) v = 0;
    if (v > 999.9) v = 999.9;
    iv = (int)(v * 10.0 + 0.5);
    d[0] = iv / 1000 % 10; d[1] = iv / 100 % 10; d[2] = iv / 10 % 10; d[3] = iv % 10;
    for (i = 0; i < 4; i++) {
        uint8_t m;
        if (i == 0 && !d[0]) { x += w + 2 * t; continue; }
        if (i == 3) {                   /* the decimal point */
            fps_rect(r, n, x, y + h - t, t, t);
            x += 2 * t;
        }
        m = seg[d[i]];
        if (m & 0x01) fps_rect(r, n, x, y, w, t);
        if (m & 0x02) fps_rect(r, n, x + w - t, y, t, h / 2 + t / 2);
        if (m & 0x04) fps_rect(r, n, x + w - t, y + h / 2, t, h / 2);
        if (m & 0x08) fps_rect(r, n, x, y + h - t, w, t);
        if (m & 0x10) fps_rect(r, n, x, y + h / 2, t, h / 2);
        if (m & 0x20) fps_rect(r, n, x, y, t, h / 2 + t / 2);
        if (m & 0x40) fps_rect(r, n, x, y + h / 2 - t / 2, w, t);
        x += w + 2 * t;
    }
    return x;
}

/* Called with dst in TRANSFER_DST_OPTIMAL; leaves it there. */
static void fps_overlay(VkImage dst, VkImageView view)
{
    static int on = -1;
    static uint64_t t_last;
    static uint32_t frames;
    static double fps, ms;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkClearAttachment cl;
    VkClearRect r[96];
    uint32_t n = 0;
    int t = s_sc_extent.height >= 1080 ? 6 : 4;
    uint64_t now = hz_now();
    if (on < 0) { const char *e = getenv("RECOMP_VK_FPS"); on = e && *e == '1'; t_last = now; }
    if (!on || !view)
        return;
    frames++;
    if (now - t_last >= 500000000ull) {
        double dt = (now - t_last) / 1e9;
        fps = frames / dt;
        ms = dt * 1000.0 / frames;
        frames = 0;
        t_last = now;
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent = s_sc_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    memset(&cl, 0, sizeof cl);
    cl.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cl.clearValue.color.float32[3] = 1.0f;          /* black panel */
    fps_rect(r, &n, 0, 0, 28 * t, 20 * t);
    vkCmdClearAttachments(s_cb, 1, &cl, n, r);
    n = 0;
    fps_number(r, &n, 2 * t, 2 * t, t, fps);
    fps_number(r, &n, 2 * t, 11 * t, t, ms);
    cl.clearValue.color.float32[0] = fps >= 59.0 ? 0.2f : fps >= 30.0 ? 1.0f : 1.0f;
    cl.clearValue.color.float32[1] = fps >= 59.0 ? 1.0f : fps >= 30.0 ? 0.85f : 0.25f;
    cl.clearValue.color.float32[2] = 0.2f;
    if (n)
        vkCmdClearAttachments(s_cb, 1, &cl, n, r);
    p_end_rendering(s_cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* A title may add a compact pause-setting row without extra textures or passes
 * during gameplay. Glyphs are 5x7 uppercase, drawn in one clear-rectangle batch. */
static void pause_option_overlay(VkImage dst, VkImageView view)
{
    const char *text = xbox_platform_pause_option ? xbox_platform_pause_option() : NULL;
    if (!text || !*text || !view) return;
    static const uint8_t font[26][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
    };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkClearAttachment clear = {0};
    VkClearRect rects[1024]; unsigned count=0;
    int scale = s_sc_extent.height >= 720 ? 2 : 1;
    size_t len = strlen(text); if (len > 48) len=48;
    int width=(int)len*6*scale, x=((int)s_sc_extent.width-width)/2;
    int y=(int)s_sc_extent.height-24*scale;
    if(x<0 || y<0) return;
    b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=dst;b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=1;
    vkCmdPipelineBarrier(s_cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&b);
    ca.imageView=view;ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent=s_sc_extent;ri.layerCount=1;ri.colorAttachmentCount=1;ri.pColorAttachments=&ca;
    p_begin_rendering(s_cb,&ri);
    clear.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;clear.clearValue.color.float32[3]=1;
    rects[0]=(VkClearRect){ .rect={{x-6*scale,y-4*scale},{(uint32_t)(width+12*scale),(uint32_t)(15*scale)}},.layerCount=1 };
    vkCmdClearAttachments(s_cb,1,&clear,1,rects);
    for(size_t i=0;i<len;i++) for(int row=0;row<7;row++) {
        unsigned bits=text[i]>='A' && text[i]<='Z' ? font[text[i]-'A'][row] :
            text[i]=='-' && row==3 ? 14 : text[i]==':' && (row==2 || row==5) ? 4 : 0;
        for(int col=0;col<5;col++) if((bits & (16u>>col)) && count<1024)
            rects[count++]=(VkClearRect){.rect={{x+((int)i*6+col)*scale,y+row*scale},{(uint32_t)scale,(uint32_t)scale}},.layerCount=1};
    }
    clear.clearValue.color.float32[0]=clear.clearValue.color.float32[1]=clear.clearValue.color.float32[2]=0.88f;
    if(count) vkCmdClearAttachments(s_cb,1,&clear,count,rects);
    p_end_rendering(s_cb);
    b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=0;
    b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(s_cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&b);
}

/* CONKER_TICKLOG=1 (diagnostic, Conker only): every 10 s, a histogram of the
 * game's per-frame logic ticks (0x7FA20C = presentation interval +
 * D3DSWAPDATA.MissedVBlanks, the latter kept at 0x7F9F30 by the swap
 * callback sub_002A8020), sampled at each flip -- why gameplay ran too fast
 * near 60 fps. */
static void tick_log(void)
{
    static int on = -1;
    static uint32_t hist_t[8], hist_m[8], n;
    static uint64_t t0;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t t, m, i;
    if (on < 0) { const char *e = getenv("CONKER_TICKLOG"); on = e && *e == '1'; t0 = hz_now(); }
    if (!on)
        return;
    memcpy(&t, mem + 0x7FA20C, 4);
    memcpy(&m, mem + 0x7F9F30, 4);
    hist_t[t < 7 ? t : 7]++;
    hist_m[m < 7 ? m : 7]++;
    n++;
    if (hz_now() - t0 >= 10000000000ull) {
        fprintf(stderr, "[ticks] %u flips: ticks 0..7+:", n);
        for (i = 0; i < 8; i++) fprintf(stderr, " %u", hist_t[i]);
        fprintf(stderr, " | missed 0..7+:");
        for (i = 0; i < 8; i++) fprintf(stderr, " %u", hist_m[i]);
        fputc(10, stderr);
        memset(hist_t, 0, sizeof hist_t);
        memset(hist_m, 0, sizeof hist_m);
        n = 0;
        t0 = hz_now();
    }
}

static void vk_flip(void)
{
    static int dump_every = -1;
    static unsigned dump_first, dump_last = UINT32_MAX;
    static unsigned dump_pause_count;
    static char dump_prefix[256];
    VkSurf *s = s_last;
    uint32_t idx = 0;
    int have_image = 0, pres_ms, pres_ss;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (s_state <= 0 && !ready())   /* ready() itself costs ~1 ms a call on the console (big frame) */
        return;
    pd_flush();
    tick_log();
    ft_start(s_frame);
    if (s_ft_on > 0) { s_ft[s_frame % FT_N].rec_end = hz_now(); s_fr_frame[s_fi] = s_frame; ft_secs(s_frame, 1); }
    s_frame++;
    {
        static uint64_t last_ns, window_ns;
        static int lines;
        uint64_t now = hz_now();
        if (last_ns) {
            uint64_t dt = now - last_ns;
            if (now - window_ns > 10000000000ull) { window_ns = now; lines = 0; }
            if (dt > 50000000ull && lines++ < 30)
                fprintf(stderr, "[hitch] frame %u: %.0f ms -- %u programs compiled (%.0f ms),"
                        " %u textures %.1f MB (%.0f ms), %u draws\n", s_frame, dt / 1e6,
                        s_hz_progs, s_hz_prog_ns / 1e6, s_hz_texs,
                        s_hz_tex_bytes / 1048576.0, s_hz_tex_ns / 1e6, s_hz_draws);
        }
        last_ns = now;
        s_hz_progs = s_hz_texs = s_hz_draws = 0;
        s_hz_prog_ns = s_hz_tex_ns = s_hz_tex_bytes = 0;
    }
    {
        static int on = -1;
        static uint32_t last_frame;
        static struct timespec last;
        struct timespec now;
        if (on < 0) {
            const char *e = getenv("RECOMP_FPS_LOG");
            on = e && *e == '1';
            clock_gettime(CLOCK_MONOTONIC, &last);
        }
        if (on) {
            double dt;
            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
            if (dt >= 10.0) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128];
                xbox_vblank_report(dt, vb, sizeof vb);
                fprintf(stderr, "[fps] %.1f fps (frame %u), %s\n",
                        (s_frame - last_frame) / dt, s_frame, vb);
                last_frame = s_frame;
                last = now;
            }
        }
    }
    if (dump_every < 0) {
        const char *dd = getenv("RECOMP_GL_DUMP");
        const char *range = getenv("RECOMP_GL_DUMP_RANGE");
        if (range && !strncmp(range, "pause:", 6)) {
            dump_pause_count = (unsigned)strtoul(range + 6, NULL, 10);
            if (!dump_pause_count || dump_pause_count > 300) dump_pause_count = 60;
            dump_first = UINT32_MAX;
        } else if (range) sscanf(range, "%u:%u", &dump_first, &dump_last);
        dump_every = 0;
        if (dd && *dd) {
            const char *comma = strchr(dd, ',');
            size_t nn = comma ? (size_t)(comma - dd) : strlen(dd);
            if (nn >= sizeof dump_prefix) nn = sizeof dump_prefix - 1;
            memcpy(dump_prefix, dd, nn);
            dump_prefix[nn] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
        }
    }
    if (dump_pause_count && xbox_platform_pause_option && xbox_platform_pause_option()) {
        dump_first = s_frame; dump_last = s_frame + dump_pause_count - 1;
        dump_pause_count = 0;
        fprintf(stderr, "[pause dump] frames %u..%u\n", dump_first, dump_last);
    }
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    if (s && s_drew_any && s_frame >= dump_first && s_frame <= dump_last && dump_every && (s_frame % (uint32_t)dump_every) == 0) {
        char path[320];
        snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
        dump_surface(s, path);
        {   /* RECOMP_SURF_DUMP=<hex va>: that render target too (diagnostic) */
            const char *sd = getenv("RECOMP_SURF_DUMP");
            VkSurf *x = sd && *sd ? surf_find((uint32_t)strtoul(sd, NULL, 16)) : NULL;
            if (x) {
                snprintf(path, sizeof path, "%s%05u_s.bmp", dump_prefix, s_frame);
                dump_surface(x, path);
            }
        }
    }
    /* The present pass reads the samples itself; the surface stays
     * unresolved (ms_dirty) for whatever samples it next. */
    const char *present_ms_env = getenv("RECOMP_VK_PRESENT_MS");
    pres_ms = !s_headless && s && s_drew_any && s->ms_image && s->ms_dirty
           && present_ms_env && *present_ms_env == '1' && pres_init(1);
    if (!pres_ms)
        surf_resolve(s);
    /* Visibility replaces the final blit. Read the resolved image so single-
     * sample post effects remain intact; no extra MSAA pass is required. */
    pres_ss = !pres_ms && !s_headless && s && s_drew_any && xbox_platform_visibility && pres_init(0);
    barrier_all();
    if (pres_ms || pres_ss)
        ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

    VT("flip %u: surface %08X drew %d, acquiring\n", s_frame, s ? s->va : 0, s_drew_any);
    if (!s_headless) {
        VkResult rr;
        nv2a_pipe_state(1, 3);
        rr = vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX, s_f->acquired,
                                   VK_NULL_HANDLE, &idx);
        nv2a_pipe_state(1, 0);
        if (rr == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        } else if (rr == VK_SUCCESS || rr == VK_SUBOPTIMAL_KHR) {
            VkImage dst = s_sc_images[idx];
            VT("flip %u: acquired image %u\n", s_frame, idx);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue black = { { 0, 0, 0, 1 } };
            have_image = 1;
            {   /* RECOMP_VK_SPLIT_PRESENT (default 1): the frame's work goes in
                 * now with no wait; only the present work below (its own
                 * command buffer) waits for the acquired image. As one
                 * submission, the acquire wait held every transfer of the
                 * frame (uploads, clears) -- and, with the multisampled
                 * present's colour-attachment stage, all of its rendering. */
                static int split = -1;
                if (split < 0) { const char *e = getenv("RECOMP_VK_SPLIT_PRESENT"); split = !(e && *e == '0'); }
                if (split) {
                    VkSubmitInfo s1 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                    vkEndCommandBuffer(s_cb);
                    s1.commandBufferCount = 1;
                    s1.pCommandBuffers = &s_cb;
                    vkQueueSubmit(s_queue, 1, &s1, VK_NULL_HANDLE);
                    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(s_f->pcb, &bi);
                    s_cb = s_f->pcb;
                    s_cb_gen++;
                    s_dyn_valid = 0;
                    barrier_all();          /* the frame's writes before the present reads */
                }
            }
            if ((pres_ms || pres_ss) && s_sc_views[idx]) {
                present_filtered(s, dst, s_sc_views[idx], pres_ms);
                goto presented;
            }
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = rg;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            vkCmdClearColorImage(s_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
            if (s && s_drew_any) {
                /* The logical aspect (16:9 for a widescreen title),
                 * letterboxed. Rows are top-first on both sides. */
                float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
                float ww = (float)s_sc_extent.width, wh = (float)s_sc_extent.height, scale;
                VkImageBlit bl;
                int dw, dh, dx, dy;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                if (xbox_video_widescreen() && lw < lh * 1.5f)
                    lw = lh * 16.0f / 9.0f;
                scale = ww / lw < wh / lh ? ww / lw : wh / lh;
                dw = (int)(lw * scale); dh = (int)(lh * scale);
                dx = ((int)ww - dw) / 2; dy = ((int)wh - dh) / 2;
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 1, &mb, 0, NULL, 0, NULL);
                memset(&bl, 0, sizeof bl);
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = 1;
                bl.srcOffsets[1].x = (int32_t)s->pw;
                bl.srcOffsets[1].y = (int32_t)s->ph;
                bl.srcOffsets[1].z = 1;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstOffsets[0].x = dx;
                bl.dstOffsets[0].y = dy;
                bl.dstOffsets[1].x = dx + dw;
                bl.dstOffsets[1].y = dy + dh;
                bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            }
            fps_overlay(dst, s_sc_views[idx]);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        presented:
            pause_option_overlay(dst, s_sc_views[idx]);
        }
    }
    VT("flip %u: submit (image %d)\n", s_frame, have_image);
    if (s_tq) {
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tq, (uint32_t)s_fi * 2 + 1);
        s_tq_valid[s_fi] = 1;
    }
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_f->acquired;
        si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_f->rendered;
    }
    {
        uint64_t t0 = hz_now();
        nv2a_pipe_state(1, 4);
        if (vkQueueSubmit(s_queue, 1, &si, s_f->fence) != VK_SUCCESS)
            LOGE("vkQueueSubmit failed\n");
        nv2a_pipe_state(1, 0);
        if (s_ft_on > 0) s_ft[(s_frame - 1) % FT_N].submit = hz_now();
        s_flp_ns[0] += hz_now() - t0;
        {   /* RECOMP_VK_SYNC=1: wait for each frame right after its submit,
             * timing the GPU by wall clock (checks the timestamp period) */
            static int sync = -1;
            if (sync < 0) { const char *e = getenv("RECOMP_VK_SYNC"); sync = e && *e == '1'; }
            if (sync) {
                vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
                s_flp_ns[3] += hz_now() - t0;
            }
        }
    }
    s_f->submitted = 1;
    if (have_image) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult pr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_f->rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &idx;
        VT("flip %u: vkQueuePresentKHR\n", s_frame);
        {
            uint64_t t0 = hz_now();
            nv2a_pipe_state(1, 5);
            pr = vkQueuePresentKHR(s_queue, &pi);
            nv2a_pipe_state(1, 0);
            s_flp_ns[1] += hz_now() - t0;
        }
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        }
    }
    {
        static uint64_t last_save, last_flush;
        uint64_t now = hz_now();
        if (now - last_flush > 5000000000ull) {
            last_flush = now;
            cache_flush();
        }
        if (s_pipes_unsaved && now - last_save > 20000000000ull) {
            last_save = now;
            pcache_save();
        }
    }
    VT("flip %u: presented, waiting for frame slot\n", s_frame);
    s_fi = (s_fi + 1) % VK_FRAMES;
    frame_begin();
    VT("flip %u: next frame recording\n", s_frame);
    s_dyn_valid = 0;
#if !defined(__SWITCH__)
    if (s_win) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) { }
    }
#endif
}

static void vk_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s; (void)b;                        /* draw_raw handles every batch */
}

static void vk_clear_timed(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                           uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    uint64_t t = hz_now();
    ft_start(s_frame);
    vk_clear(sf, rs, flags, argb, zstencil);
    s_sec_ns[SEC_CLEAR] += hz_now() - t;
}

static void vk_flip_timed(void)
{
    uint64_t t = hz_now();
    vk_flip();
    s_sec_ns[SEC_FLIP] += hz_now() - t;
    if (s_ft_on > 0) { s_ft[(s_frame - 1) % FT_N].flip_end = hz_now(); s_ft_open = 0; }
    s_sec_frames++;
}

static const Nv2aBackend s_backend = {
    vk_clear_timed,
    vk_draw,
    vk_flip_timed,
    vk_draw_raw,
    NV2A_BACKEND_RAW_DIRECT | NV2A_BACKEND_RAW_CMP,
};

void nv2a_vk_install(void)
{
    nv2a_backend_register(nv2a_fixture_wrap(nv2a_queue_wrap(&s_backend, "RECOMP_VK_THREAD")));
    /* Device, caches and prewarm now, before the title starts: prewarm on the
     * push-buffer thread blocked it inside the title's D3D init, which waits
     * on that thread (fences, flips) -- with caches on the console never got
     * past the boot movie (any prewarm length; replays, which nothing waits
     * on, were fine with the same caches). */
    if (!getenv("RECOMP_REPLAY")) {
        s_prewarm_early = 1;
        ready();
    }
    fprintf(stderr, "[BOOT] NV2A Vulkan renderer registered\n");
}

int nv2a_renderer_replay(void)
{
    return nv2a_fixture_replay_main(nv2a_queue_wrap(&s_backend, "RECOMP_VK_THREAD"));
}

/* Between fixtures in one replay job: every surface, depth buffer and cached
 * texture goes (the next fixture would otherwise sample the previous one's
 * render targets at the same guest addresses); programs, pipelines and
 * samplers stay, so later fixtures still start warm. */
void nv2a_vk_reset_resources(void)
{
    int i;
    if (!s_dev)
        return;
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    s_last = NULL;
    vkDeviceWaitIdle(s_dev);
    for (i = 0; i < VK_MAX_SURF; i++) {
        VkSurf *x = &s_surf[i];
        if (x->view_a1) vkDestroyImageView(s_dev, x->view_a1, NULL);
        if (x->view) vkDestroyImageView(s_dev, x->view, NULL);
        if (x->image) vkDestroyImage(s_dev, x->image, NULL);
        if (x->mem) vkFreeMemory(s_dev, x->mem, NULL);
        if (x->ms_view) vkDestroyImageView(s_dev, x->ms_view, NULL);
        if (x->ms_image) vkDestroyImage(s_dev, x->ms_image, NULL);
        if (x->ms_mem) vkFreeMemory(s_dev, x->ms_mem, NULL);
        memset(x, 0, sizeof *x);
    }
    for (i = 0; i < VK_MAX_DEPTH; i++) {
        VkDepthBuf *d = &s_depth[i];
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->image) vkDestroyImage(s_dev, d->image, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        memset(d, 0, sizeof *d);
    }
    for (i = 0; i < VK_MAX_TEX; i++)
        if (s_tex[i].image)
            tex_free(&s_tex[i]);
    s_dyn_valid = 0;
    s_regs = NULL;          /* pointed into the previous fixture's records */
    s_batch = NULL;
}

/* Everything the renderer created, destroyed, then the device and instance.
 * A bench job calls it before handing the console back to hbloader: with the
 * device alive, NVK's buffers stayed mapped into the GPU's address space over
 * heap memory hbloader then reused -- the crash after every replay. */
void nv2a_vk_shutdown(void)
{
    int i;
    uint32_t j;
    nv2a_queue_shutdown();
    cache_writer_shutdown();
    if (!s_dev)
        return;
    if (s_in_render)
        end_rendering();
    vkDeviceWaitIdle(s_dev);
    cache_writer_drain();
    cache_flush_sync();
    if (s_pipes_unsaved)
        pcache_save_sync();
    for (i = 0; i < VK_FRAMES; i++) {
        Frame *f = &s_fr[i];
        garbage_free(f);
        free(f->garbage);
        f->garbage = NULL;
        f->garbage_cap = 0;
        if (f->iring) vkDestroyBuffer(s_dev, f->iring, NULL);
        if (f->iring_mem) vkFreeMemory(s_dev, f->iring_mem, NULL);
        if (f->ring) vkDestroyBuffer(s_dev, f->ring, NULL);
        if (f->ring_mem) vkFreeMemory(s_dev, f->ring_mem, NULL);
        if (f->fence) vkDestroyFence(s_dev, f->fence, NULL);
        if (f->acquired) vkDestroySemaphore(s_dev, f->acquired, NULL);
        if (f->rendered) vkDestroySemaphore(s_dev, f->rendered, NULL);
        if (f->pool) vkDestroyCommandPool(s_dev, f->pool, NULL);
        memset(f, 0, sizeof *f);
    }
    for (i = 0; i < VK_MAX_SURF; i++) {
        VkSurf *x = &s_surf[i];
        if (x->view_a1) vkDestroyImageView(s_dev, x->view_a1, NULL);
        if (x->view) vkDestroyImageView(s_dev, x->view, NULL);
        if (x->image) vkDestroyImage(s_dev, x->image, NULL);
        if (x->mem) vkFreeMemory(s_dev, x->mem, NULL);
        if (x->ms_view) vkDestroyImageView(s_dev, x->ms_view, NULL);
        if (x->ms_image) vkDestroyImage(s_dev, x->ms_image, NULL);
        if (x->ms_mem) vkFreeMemory(s_dev, x->ms_mem, NULL);
        memset(x, 0, sizeof *x);
    }
    for (i = 0; i < VK_MAX_DEPTH; i++) {
        VkDepthBuf *d = &s_depth[i];
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->image) vkDestroyImage(s_dev, d->image, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        memset(d, 0, sizeof *d);
    }
    for (i = 0; i < VK_MAX_TEX; i++) {
        VkTex *t = &s_tex[i];
        if (t->view) vkDestroyImageView(s_dev, t->view, NULL);
        if (t->image) vkDestroyImage(s_dev, t->image, NULL);
        if (t->mem) vkFreeMemory(s_dev, t->mem, NULL);
        memset(t, 0, sizeof *t);
    }
    if (s_dummy_view) vkDestroyImageView(s_dev, s_dummy_view, NULL);
    if (s_dummy_img) vkDestroyImage(s_dev, s_dummy_img, NULL);
    if (s_dummy_mem) vkFreeMemory(s_dev, s_dummy_mem, NULL);
    if (s_dummy_cube_view) vkDestroyImageView(s_dev, s_dummy_cube_view, NULL);
    if (s_dummy_cube_img) vkDestroyImage(s_dev, s_dummy_cube_img, NULL);
    if (s_dummy_cube_mem) vkFreeMemory(s_dev, s_dummy_cube_mem, NULL);
    for (j = 0; j < (uint32_t)VK_MAX_PIPE; j++)
        if (s_pipes[j].pipe) vkDestroyPipeline(s_dev, s_pipes[j].pipe, NULL);
    for (j = 0; j < (uint32_t)VK_MAX_PROG; j++) {
        if (s_prog[j].vs) vkDestroyShaderModule(s_dev, s_prog[j].vs, NULL);
        if (s_prog[j].fs) vkDestroyShaderModule(s_dev, s_prog[j].fs, NULL);
    }
    for (i = 0; i < 64; i++)
        if (s_samplers[i].s) vkDestroySampler(s_dev, s_samplers[i].s, NULL);
    if (s_tq) vkDestroyQueryPool(s_dev, s_tq, NULL);
    if (s_pp) vkDestroyQueryPool(s_dev, s_pp, NULL);
    s_pp = VK_NULL_HANDLE;
    s_pp_on = -1;
    s_tq = VK_NULL_HANDLE;
    if (s_layout) vkDestroyPipelineLayout(s_dev, s_layout, NULL);
    if (s_dsl) vkDestroyDescriptorSetLayout(s_dev, s_dsl, NULL);
    if (s_ubo_pool) vkDestroyDescriptorPool(s_dev, s_ubo_pool, NULL);
    if (s_pres_pipe) vkDestroyPipeline(s_dev, s_pres_pipe, NULL);
    if (s_pres_ss_pipe) vkDestroyPipeline(s_dev, s_pres_ss_pipe, NULL);
    if (s_pres_layout) vkDestroyPipelineLayout(s_dev, s_pres_layout, NULL);
    s_pres_pipe = s_pres_ss_pipe = VK_NULL_HANDLE;
    s_pres_layout = VK_NULL_HANDLE; s_pres = -1;
    s_pres_fmt = s_pres_ss_fmt = VK_FORMAT_UNDEFINED;
    if (s_sres_pipe) vkDestroyPipeline(s_dev, s_sres_pipe, NULL);
    if (s_sres_layout) vkDestroyPipelineLayout(s_dev, s_sres_layout, NULL);
    if (s_sres_dsl) vkDestroyDescriptorSetLayout(s_dev, s_sres_dsl, NULL);
    if (s_sres_sampler) vkDestroySampler(s_dev, s_sres_sampler, NULL);
    s_sres_pipe = VK_NULL_HANDLE; s_sres_layout = VK_NULL_HANDLE;
    s_sres_dsl = VK_NULL_HANDLE; s_sres_sampler = VK_NULL_HANDLE; s_sres = -1;
    if (s_dsl_ubo) vkDestroyDescriptorSetLayout(s_dev, s_dsl_ubo, NULL);
    s_ubo_pool = VK_NULL_HANDLE;
    s_dsl_ubo = VK_NULL_HANDLE;
    if (s_pcache) vkDestroyPipelineCache(s_dev, s_pcache, NULL);
    if (s_swapchain) vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
    vkDestroyDevice(s_dev, NULL);
    s_dev = VK_NULL_HANDLE;
    if (s_surface) vkDestroySurfaceKHR(s_inst, s_surface, NULL);
    if (s_inst) vkDestroyInstance(s_inst, NULL);
    s_inst = VK_NULL_HANDLE;
    fprintf(stderr, "  [VK] device destroyed\n");
}
