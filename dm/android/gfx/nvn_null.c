// Null graphics device for the Android host: the interface the NVN layer (dm/gfx/nvn/hle_nvn.c) calls in nvn_mtl.m, with no GPU. It lets a game run its frame loop with
// nothing drawn, until the Vulkan backend takes its place.
//   · Submissions complete at once. The replay walks the recorded commands (nvn_obj.h) and performs those with CPU-visible results as nvn_mtl.m's completion handler
//     would: event writes and adds, counter reports ({counter, timestamp} in GPU ticks from the guest clock; timestamps also in the counter field), fence syncs, buffer fills,
//     buffer copies and inline uniform updates (GPU addresses are CPU addresses). Draws, dispatches, clears and texture copies do nothing: a compute shader's buffer results
//     are not produced, and occlusion queries report no samples.
//   · There is no window: presents return at once and are not shown (mtl_has_window 0), so the NVN layer paces frames on the host clock.
//   · Memory pools are tracked so mtl_addr finds the pool of a GPU address, as shader code mapping needs.
//   · Statistics read zero.
#include "../../host/hle.h"
#include "../../gfx/nvn/nvn_obj.h"
#include "../../gfx/nvn/nvn_mtl.h"
#include "../../gfx/nvn/nvn_timestamp.h"

static uint64_t g_fence;   // last committed submission (all complete)
static int g_red_dispatch;

// ---- Memory pools ----
#define NPOOL 4096
static struct { uint64_t base, size; } g_pool[NPOOL]; static int g_npool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool < NPOOL) { g_pool[g_npool].base = (uint64_t)(uintptr_t)mem; g_pool[g_npool].size = size; g_npool++; }
    else LOG("[RED] null graphics: more than %d memory pools, %p not tracked", NPOOL, mem);
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].base == (uint64_t)(uintptr_t)mem) { g_pool[i] = g_pool[--g_npool]; break; }
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool && !ok; i++) if (addr >= g_pool[i].base && addr + len <= g_pool[i].base + g_pool[i].size) {
        if (buf) *buf = (void *)(uintptr_t)g_pool[i].base; if (off) *off = addr - g_pool[i].base; ok = 1; }
    pthread_mutex_unlock(&g_pool_mu);
    return ok;
}

// ---- Submission: the CPU-visible commands ----
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) g = now_ns(); return nvn_timestamp_from_ns(g); }
void mtl_sub_begin(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_exec(const uint8_t *p) {
    for (int guard = 0; guard < 1 << 24; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
                if (m == 1) *val += v; else *val = v; break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
                uint64_t ts = gpu_ticks(), rec[2] = {t == 0 || t == 0x10 ? ts : 0, ts};
                if (ad) memcpy((void *)(uintptr_t)ad, rec, sizeof rec);
                break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; uint32_t *d = (uint32_t *)(uintptr_t)cb->addr;
                for (uint64_t i = 0; i < cb->size / 4; i++) d[i] = cb->value; break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
                memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s_, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)(uintptr_t)d, a + 16, n); break; }
            case OP_DISPATCH: if (!g_red_dispatch++) LOG("[RED] null graphics: compute dispatches do not run (first one here); their buffer results are missing"); break;
            default: break;
        }
        if (h.size < 8) { LOG("[RED] null graphics: command record of size %u at %p (op %u), submission cut", h.size, (const void *)p, h.op); return; }
        p += h.size;
    }
    LOG("[RED] null graphics: a command list did not end within 16M records, submission cut");
}
uint64_t mtl_sub_commit(void) { return ++g_fence; }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}

// ---- Objects ----
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0;
    size_t rows = (size_t)(r->height > 0 ? r->height : 1), depth = (size_t)(r->depth > 0 ? r->depth : 1);
    for (size_t z = 0; z < depth; z++) for (size_t y = 0; y < rows; y++) memset(dst + z * img + y * row, 0, row);   // a texture with no GPU behind it reads as zeros
}
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// ---- Presentation: no window ----
static uint64_t g_ticket;
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) { (void)t; (void)period_s; (void)crop; return ++g_ticket; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_fps_target(void) { static int v; if (!v) { const char *e = getenv("DM_FPS"); v = e && atoi(e) == 30 ? 30 : 60; } return v; }
int mtl_link_active(void) { return 0; }
int mtl_link_rate(void) { return 60; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
int mtl_pso_prebuild_tool(void) { return 0; }

// ---- Diagnostics ----
void dm_gfx_shot_at(long k, const char *path) { (void)k; LOG("[RED] null graphics: no screenshot of %s (nothing is drawn)", path); }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { LOG("[nvn] memory at frame %llu: %llu MB resident (null graphics)", frame, mb); }
void mtl_res_stats(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_ubp_why(uint64_t w[5]) { for (int i = 0; i < 5; i++) w[i] = 0; }
#define Z1(f) void f(uint64_t *a) { *a = 0; }
#define Z2(f) void f(uint64_t *a, uint64_t *b) { *a = *b = 0; }
#define Z3(f) void f(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
#define ZR(f) uint64_t f(void) { return 0; }
Z2(mtl_args_stats) Z2(mtl_barrier_stats) Z3(mtl_bw) Z2(mtl_create_ns) Z2(mtl_create_stats) Z3(mtl_cshoist_stats) Z3(mtl_hybrid_stats) Z2(mtl_lc_stats) Z2(mtl_pkg_stats)
Z3(mtl_sm_stats) Z2(mtl_stats) Z2(mtl_ubd_stats) Z3(mtl_ubp_stats) Z2(mtl_use_stats)
ZR(mtl_blit_splits) ZR(mtl_copy_cut_count) ZR(mtl_cut_count) ZR(mtl_ds_omit) ZR(mtl_pkg_edge_skips) ZR(mtl_pkg_miss) ZR(mtl_udc_saved_bytes) ZR(mtl_upload_bytes)
ZR(mtl_upload_gpu_ns_take) ZR(mtl_upload_ns_take)
unsigned mtl_cpu_tex_count(void) { return 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { *resolve = *reuse = *set = *skip = *copies = *copy_bytes = 0; }
