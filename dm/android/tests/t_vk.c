// Tests of the Vulkan backend (dm/android/gfx/nvn_vk.c) on any Vulkan 1.1 device: on a desktop with Mesa's lavapipe (run_vk.sh builds it for the host), or on a phone.
// Command lists are recorded here as hle_nvn.c records them (nvn_obj.h) and replayed through mtl_sub_begin / mtl_exec / mtl_sub_commit; results are read back with texture-to-buffer
// copies and the presented frame (no window: nvn_vk_test_frame). With DM_VK_VALIDATE=1 any validation error fails the run. Argument "nobc": BC textures decoded on the CPU.
#include "../../gfx/nvn/nvn_obj.h"
#include "../../gfx/nvn/nvn_mtl.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t guest_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void *front_metal_layer(void) { return NULL; }
// What hle_nvn.c gives the backend: a program's shader keys (here the first 8 bytes of prog_t.sd[i] hold the key) and the descriptor pool bases (one pool each, base 0)
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) { memcpy(key, &p->sd[st], 8); return 0; }
int nvn_dpool_base(const void *pool, int tex) { (void)pool; (void)tex; return 0; }
int nvn_vk_test_frame(uint8_t *rgba, uint32_t w, uint32_t h);
extern atomic_int nvn_vk_validation_errors;

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail++; } printf("\n"); } while (0)

// ---- command list builder ----
static uint8_t g_cmd[1 << 16]; static size_t g_n;
static void *rec(uint32_t op, size_t payload) {
    uint32_t size = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    op_hdr h = {op, size}; memcpy(g_cmd + g_n, &h, 8); memset(g_cmd + g_n + 8, 0, size - 8);
    void *p = g_cmd + g_n + 8; g_n += size; return p;
}
static void run(void) { rec(OP_END, 0); mtl_sub_begin(); mtl_exec(g_cmd); mtl_sub_commit(); g_n = 0; }

static uint64_t g_serial = 100;
static tex_t mk(int format, uint32_t w, uint32_t h, int target, uint32_t depth) {
    tex_t t; memset(&t, 0, sizeof t); t.serial = ++g_serial;
    t.d.target = target; t.d.format = format; t.d.width = w; t.d.height = h; t.d.depth = depth ? depth : 1; t.d.levels = 1;
    return t;
}
static void set_rt(const tex_t *color) { p_set_rt *r = rec(OP_SET_RT, sizeof *r); r->n = 1; r->color[0] = color; }
static void scissor(int x, int y, int w, int h) { p_scissor *s = rec(OP_SCISSOR, sizeof *s); *s = (p_scissor){x, y, w, h}; }
static void clear(float r, float g, float b, float a) { p_clear_color *c = rec(OP_CLEAR_COLOR, sizeof *c); c->index = 0; c->mask = 0xf; c->rgba[0] = r; c->rgba[1] = g; c->rgba[2] = b; c->rgba[3] = a; }
static void b2t(const tex_t *t, const void *src, int x, int y, int z, int w, int h, int d) {
    p_copy_b2t *c = rec(OP_COPY_B2T, sizeof *c); c->src = (uint64_t)(uintptr_t)src; c->tex = t; c->r = (NVNcopyRegion){x, y, z, w, h, d};
}
static void t2b(const tex_t *t, void *dst, int x, int y, int z, int w, int h, int d) {
    p_copy_b2t *c = rec(OP_COPY_T2B, sizeof *c); c->src = (uint64_t)(uintptr_t)dst; c->tex = t; c->r = (NVNcopyRegion){x, y, z, w, h, d};
}
static void stride(int64_t s) { p_copy_stride *c = rec(OP_COPY_STRIDE, sizeof *c); c->stride = s; c->image = 0; }
static uint32_t px(const uint8_t *img, int w, int x, int y) { uint32_t v; memcpy(&v, img + 4 * ((size_t)y * w + x), 4); return v; }

static void test_clear_scissor(void) {
    tex_t rt = mk(0x25, 64, 32, NVN_TEXTURE_TARGET_2D, 1);   // RGBA8
    static uint8_t out[64 * 32 * 4];
    set_rt(&rt); scissor(0, 0, 64, 32); clear(0, 0, 1, 1);    // all blue
    scissor(8, 4, 16, 8); clear(1, 0, 0, 1);                 // a red rectangle
    t2b(&rt, out, 0, 0, 0, 64, 32, 1); run();
    CHECK(px(out, 64, 10, 5) == 0xff0000ffu && px(out, 64, 23, 11) == 0xff0000ffu, "scissored clear: red inside (%#x)", px(out, 64, 10, 5));
    CHECK(px(out, 64, 7, 5) == 0xffff0000u && px(out, 64, 24, 11) == 0xffff0000u && px(out, 64, 10, 3) == 0xffff0000u && px(out, 64, 10, 12) == 0xffff0000u, "scissored clear: blue outside (%#x)", px(out, 64, 7, 5));
}
static void test_b2t_t2b(void) {
    tex_t t = mk(0x25, 40, 20, NVN_TEXTURE_TARGET_2D, 1);
    static uint8_t src[48 * 20 * 4], out[30 * 10 * 4];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7 + 3);
    stride(48 * 4); b2t(&t, src, 0, 0, 0, 40, 20, 1);         // rows of 48 texels in the buffer
    stride(0); t2b(&t, out, 5, 3, 0, 30, 10, 1); run();        // a 30x10 rectangle back, tight
    int ok = 1; for (int y = 0; y < 10; y++) ok &= !memcmp(out + (size_t)y * 30 * 4, src + ((size_t)(y + 3) * 48 + 5) * 4, 30 * 4);
    CHECK(ok, "buffer → texture (row stride 192) → buffer: the sub-rectangle round-trips");
}
static void test_packaged_and_present(void) {
    // a 64x64 RGBA8 packaged texture: block-linear storage made from a linear pattern
    tex_t t = mk(0x25, 64, 64, NVN_TEXTURE_TARGET_2D, 1); t.has_layout = 1;
    nvn_texlayout L; nvn_tex_layout(&t.d, &L);
    static uint8_t lin[64 * 64 * 4]; uint8_t *store = calloc(1, L.size); t.mem = store;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint32_t v = 0xff000000u | (uint32_t)(y * 4) << 8 | (uint32_t)(x * 4); memcpy(lin + 4 * (y * 64 + x), &v, 4); }
    nvn_swizzle(&t.d, 0, 0, lin, 64 * 4, store);
    CHECK(memcmp(store, lin, 64 * 4) != 0, "the packaged storage is block-linear (not the linear rows)");
    static uint8_t out[64 * 64 * 4];
    t2b(&t, out, 0, 0, 0, 64, 64, 1); run();
    CHECK(!memcmp(out, lin, sizeof lin), "packaged texture: deswizzled on upload, reads back as the linear pattern");
    // texture → texture copy, then a mirrored blit
    tex_t d2 = mk(0x25, 64, 64, NVN_TEXTURE_TARGET_2D, 1);
    p_copy_t2t *c = rec(OP_COPY_T2T, sizeof *c); c->src = &t; c->dst = &d2; c->sr = c->dr = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    static uint8_t o2[64 * 64 * 4]; t2b(&d2, o2, 0, 0, 0, 64, 64, 1); run();
    CHECK(!memcmp(o2, lin, sizeof lin), "texture → texture copy");
    tex_t d3 = mk(0x25, 64, 64, NVN_TEXTURE_TARGET_2D, 1);
    c = rec(OP_COPY_T2T, sizeof *c); c->src = &t; c->dst = &d3; c->sr = c->dr = (NVNcopyRegion){0, 0, 0, 64, 64, 1}; c->flags = NVN_COPY_FLAGS_MIRROR_X_BIT;
    t2b(&d3, o2, 0, 0, 0, 64, 64, 1); run();
    CHECK(px(o2, 64, 0, 9) == px(lin, 64, 63, 9) && px(o2, 64, 63, 9) == px(lin, 64, 0, 9), "mirrored copy (blit)");
    // present: lower-left origin (NVN's default) flips the picture; the crop takes a part of it
    static uint8_t fr[64 * 64 * 4];
    mtl_present(&t, 0, NULL);
    CHECK(nvn_vk_test_frame(fr, 64, 64) && px(fr, 64, 5, 0) == px(lin, 64, 5, 63) && px(fr, 64, 5, 63) == px(lin, 64, 5, 0), "present, lower-left origin: the frame is the texture flipped");
    mtl_set_origin_lower_left(0);
    int crop[4] = {16, 8, 32, 16}; static uint8_t fc[32 * 16 * 4];
    mtl_present(&t, 0, crop);
    CHECK(nvn_vk_test_frame(fc, 32, 16) && px(fc, 32, 0, 0) == px(lin, 64, 16, 8) && px(fc, 32, 31, 15) == px(lin, 64, 47, 23), "present, upper-left origin, cropped to 32x16 at (16, 8)");
    mtl_set_origin_lower_left(1);
    free(store);
}
static int g_nobc;
static void test_bc1(void) {
    // BC1, 8x8: four blocks, each a solid colour (colour 0 = colour 1, all indices 0)
    tex_t t = mk(0x42, 8, 8, NVN_TEXTURE_TARGET_2D, 1);   // RGBA_DXT1
    uint16_t c565[4] = {0xf800, 0x07e0, 0x001f, 0xffff};   // red, green, blue, white
    uint8_t blocks[4][8]; for (int i = 0; i < 4; i++) { memset(blocks[i], 0, 8); memcpy(blocks[i], &c565[i], 2); memcpy(blocks[i] + 2, &c565[i], 2); }
    b2t(&t, blocks, 0, 0, 0, 8, 8, 1);
    if (!g_nobc) {   // the device samples BC: the blocks are stored as they are (decoding them is the sampler's, which stage 1 does not draw with)
        uint8_t back[4][8]; t2b(&t, back, 0, 0, 0, 8, 8, 1); run();
        CHECK(!memcmp(back, blocks, sizeof blocks), "BC1 blocks uploaded as they are");
        mtl_present(&t, 0, NULL);
        CHECK(1, "present of a BC texture is refused without a crash");
        return;
    }
    run();
    mtl_set_origin_lower_left(0);
    static uint8_t fr[8 * 8 * 4]; mtl_present(&t, 0, NULL); int got = nvn_vk_test_frame(fr, 8, 8);
    mtl_set_origin_lower_left(1);
    CHECK(got && px(fr, 8, 1, 1) == 0xff0000ffu && px(fr, 8, 5, 1) == 0xff00ff00u && px(fr, 8, 1, 5) == 0xffff0000u && px(fr, 8, 6, 6) == 0xffffffffu,
          "BC1 blocks: red / green / blue / white (%#x %#x %#x %#x)", px(fr, 8, 1, 1), px(fr, 8, 5, 1), px(fr, 8, 1, 5), px(fr, 8, 6, 6));
}
static void test_events_counters(void) {
    static volatile uint32_t ev = 5; static uint64_t ctr[2];
    struct { volatile uint32_t *val; uint32_t m, v; } *e = rec(OP_SIGNAL_EVENT, 16); e->val = &ev; e->m = 1; e->v = 3;   // add 3
    uint64_t *r = rec(OP_REPORT_COUNTER, 16); r[0] = 0x10; r[1] = (uint64_t)(uintptr_t)ctr;   // TIMESTAMP_TOP
    sync_t sy = {0}; const sync_t *ps = &sy; void *f = rec(OP_FENCE_SYNC, 8); memcpy(f, &ps, 8);
    uint64_t before = mtl_last_fence(); run();
    CHECK(ev == 8, "event add (5 + 3 = %u)", ev);
    CHECK(ctr[0] && ctr[0] == ctr[1], "timestamp report written (%#llx)", (unsigned long long)ctr[1]);
    CHECK(sy.fenced == 1 && sy.fence == before + 1 && mtl_last_fence() == before + 1 && mtl_wait(sy.fence, 0), "fence sync completes with its submission");
    // buffer writes stay in order with a readback before them in the same list
    tex_t t = mk(0x25, 4, 1, NVN_TEXTURE_TARGET_2D, 1); uint32_t px4[4] = {1, 2, 3, 4}, back[4] = {0}, copy[4] = {0};
    b2t(&t, px4, 0, 0, 0, 4, 1, 1); t2b(&t, back, 0, 0, 0, 4, 1, 1);
    uint64_t *bb = rec(OP_COPY_B2B, 24); bb[0] = (uint64_t)(uintptr_t)back; bb[1] = (uint64_t)(uintptr_t)copy; bb[2] = 16;
    run();
    CHECK(!memcmp(back, px4, 16) && !memcmp(copy, px4, 16), "a buffer copy after a readback sees the read-back data");
}
static void test_array_and_depth(void) {
    tex_t a = mk(0x25, 16, 16, NVN_TEXTURE_TARGET_2D_ARRAY, 3);
    p_clear_tex *c = rec(OP_CLEAR_TEX, sizeof *c); c->tex = &a; c->r = (NVNcopyRegion){0, 0, 1, 16, 16, 1}; c->mask = 0xf; c->c.f[1] = 1; c->c.f[3] = 1;   // layer 1 green
    static uint8_t out[16 * 16 * 4 * 3]; t2b(&a, out, 0, 0, 0, 16, 16, 3); run();
    CHECK(px(out, 16, 3, 3) == 0 && px(out + 16 * 16 * 4, 16, 3, 3) == 0xff00ff00u && px(out + 2 * 16 * 16 * 4, 16, 3, 3) == 0, "ClearTexture of layer 1 of a 2D array");
    tex_t dz = mk(0x34, 32, 32, NVN_TEXTURE_TARGET_2D, 1);   // DEPTH32F
    p_set_rt *r = rec(OP_SET_RT, sizeof *r); r->n = 0; r->depth = &dz; scissor(0, 0, 32, 32);
    p_clear_ds *d = rec(OP_CLEAR_DS, sizeof *d); d->depth = 0.25f; d->depth_mask = 1;
    float dv[32 * 32]; t2b(&dz, dv, 0, 0, 0, 32, 32, 1); run();
    CHECK(dv[0] == 0.25f && dv[32 * 32 - 1] == 0.25f, "depth clear 0.25 (%g)", dv[0]);
}
// ---- draws (stage 2): translator-shaped shaders (tests/shaders, keys 1–5, run_vk.sh) ----
static uint8_t g_mem[1 << 20] __attribute__((aligned(4096))); static size_t g_mtop;   // "guest memory": one registered memory pool
static void *gmem(size_t n) { void *p = g_mem + g_mtop; g_mtop = (g_mtop + n + 255) & ~(size_t)255; memset(p, 0, n); return p; }
static prog_t g_pr[5];
static void prog_init(prog_t *p, int nst, int k0, int k1) {
    memset(p, 0, sizeof *p); p->nstage = (uint32_t)nst; p->serial = ++g_serial;
    uint8_t key[8] = {0}; key[7] = (uint8_t)k0; memcpy(&p->sd[0], key, 8); key[7] = (uint8_t)k1; if (nst > 1) memcpy(&p->sd[1], key, 8);
}
static void bind_prog(const prog_t *p, int stages) { p_bind_program *b = rec(OP_BIND_PROGRAM, sizeof *b); b->prog = p; b->stages = stages; }
static void viewport(float x, float y, float w, float h) { p_viewports *v = rec(OP_VIEWPORTS, sizeof *v); v->first = 0; v->n = 1; v->v[0][0] = x; v->v[0][1] = y; v->v[0][2] = w; v->v[0][3] = h; v->v[0][5] = 1; }
static void vertices(const float *v, int n) {   // n vertices of (x, y, z, w, r, g, b, a): attribute 0 position, 1 colour, from stream 0
    float *m = gmem((size_t)n * 32); memcpy(m, v, (size_t)n * 32);
    p_bind_vattr *va = rec(OP_BIND_VATTR, sizeof *va); va->first = 0; va->n = 2;
    va_st a0 = {0x2E, 0, 0}, a1 = {0x2E, 16, 0}; memcpy(&va->st[0], &a0, 4); memcpy(&va->st[1], &a1, 4);   // RGBA32F
    p_bind_vstream *vs = rec(OP_BIND_VSTREAM, sizeof *vs); vs->first = 0; vs->n = 1; vs->st[0] = (vs_st){32, 0};
    p_bind_vbuf *vb = rec(OP_BIND_VBUF, sizeof *vb); vb->first = 0; vb->n = 1; vb->r[0] = (NVNbufferRange){(uint64_t)(uintptr_t)m, (uint64_t)n * 32};
}
static void draw(int prim, int first, int count) { p_draw *d = rec(OP_DRAW, sizeof *d); d->prim = prim; d->first = first; d->count = count; d->instances = 1; }
static void state(int kind, const void *v, size_t n) { p_bind_state *b = rec(OP_BIND_STATE, sizeof *b); b->kind = kind; memcpy(b->bytes, v, n); }
#define QUAD(x0, y0, x1, y1, z, r, g, b, a) {x0, y0, z, 1, r, g, b, a}, {x1, y0, z, 1, r, g, b, a}, {x1, y1, z, 1, r, g, b, a}, {x0, y1, z, 1, r, g, b, a}
static int all(const uint8_t *img, int w, int x0, int y0, int x1, int y1, uint32_t v) { for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (px(img, w, x, y) != v) return 0; return 1; }
static void test_draws(void) {
    mtl_pool_add(g_mem, sizeof g_mem);
    prog_init(&g_pr[0], 2, 1, 2); prog_init(&g_pr[1], 2, 1, 3); prog_init(&g_pr[2], 2, 1, 4); prog_init(&g_pr[3], 1, 5, 0);
    tex_t rt = mk(0x25, 32, 32, NVN_TEXTURE_TARGET_2D, 1); static uint8_t out[32 * 32 * 4];
    // quads with vertex colours: the whole target red
    float q1[4][8] = {QUAD(-1, -1, 1, 1, 0, 1, 0, 0, 1)};
    set_rt(&rt); scissor(0, 0, 32, 32); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&q1[0][0], 4); draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4);
    t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    CHECK(all(out, 32, 0, 0, 32, 32, 0xff0000ffu), "draw: a red quad covers the target (%#x)", px(out, 32, 16, 16));
    // origins: NDC y in [-1, 0] fills memory rows 0–15 with the lower-left origin (GL), rows 16–31 with the upper-left one
    float q2[4][8] = {QUAD(-1, -1, 1, 0, 0, 0, 1, 0, 1)};
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&q2[0][0], 4); draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    CHECK(all(out, 32, 0, 0, 32, 16, 0xff00ff00u) && all(out, 32, 0, 16, 32, 32, 0xff000000u), "lower-left origin: NDC bottom half → memory rows 0–15");
    mtl_set_origin_lower_left(0);
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&q2[0][0], 4); draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    mtl_set_origin_lower_left(1);
    CHECK(all(out, 32, 0, 16, 32, 32, 0xff00ff00u) && all(out, 32, 0, 0, 32, 16, 0xff000000u), "upper-left origin: NDC bottom half → memory rows 16–31");
    // a uniform buffer of the pixel stage (cbuf slot 3)
    float *tint = gmem(16); tint[0] = 0.25f; tint[1] = 0.5f; tint[2] = 0.75f; tint[3] = 1;
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[1], 3); vertices(&q1[0][0], 4);
    p_bind_buf *u = rec(OP_BIND_UBO, sizeof *u); *u = (p_bind_buf){1, 0, (uint64_t)(uintptr_t)tint, 16};
    draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    uint32_t c = px(out, 32, 9, 9);
    CHECK((c & 0xff) >= 63 && (c & 0xff) <= 65 && (c >> 8 & 0xff) >= 127 && (c >> 8 & 0xff) <= 128 && (c >> 16 & 0xff) >= 190 && (c >> 16 & 0xff) <= 192, "uniform buffer of the pixel stage: tint (%#x)", c);
    // a texture through the registered pools: texture id 5, sampler id 2 (nearest), handle 5 | 2 << 20 in combined slot 0
    tex_t tx = mk(0x25, 2, 2, NVN_TEXTURE_TARGET_2D, 1); uint32_t texel[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    b2t(&tx, texel, 0, 0, 0, 2, 2, 1); run();
    mtl_reg_tex(5, &tx, NULL);
    samp_par sp; memset(&sp, 0, sizeof sp); sp.minf = NVN_MIN_FILTER_NEAREST; sp.magf = NVN_MAG_FILTER_NEAREST; sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; sp.lod_max = 1000; sp.aniso = 1;
    mtl_reg_samp(2, &sp);
    float q3[4][8] = {{-1, -1, 0, 1, 0, 0, 0, 1}, {1, -1, 0, 1, 1, 0, 0, 1}, {1, 1, 0, 1, 1, 1, 0, 1}, {-1, 1, 0, 1, 0, 1, 0, 1}};   // colour = uv
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[2], 3); vertices(&q3[0][0], 4);
    p_bind_handles *bh = rec(OP_BIND_TEX, sizeof *bh); bh->stage = 1; bh->first = 0; bh->n = 1; bh->h[0] = 5 | 2u << 20;
    draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    CHECK(px(out, 32, 4, 4) == texel[0] && px(out, 32, 28, 4) == texel[1] && px(out, 32, 4, 28) == texel[2] && px(out, 32, 28, 28) == texel[3],
          "texture from the pools (nearest): the four texels in their quadrants (%#x %#x %#x %#x)", px(out, 32, 4, 4), px(out, 32, 28, 4), px(out, 32, 4, 28), px(out, 32, 28, 28));
    // indexed, 8-bit indices and a base vertex: two triangles of vertices 4–7
    float q4[8][8] = {{0}, {0}, {0}, {0}, {-1, -1, 0, 1, 0, 0, 1, 1}, {1, -1, 0, 1, 0, 0, 1, 1}, {1, 1, 0, 1, 0, 0, 1, 1}, {-1, 1, 0, 1, 0, 0, 1, 1}};
    uint8_t *ix = gmem(8); const uint8_t ixs[6] = {0, 1, 2, 0, 2, 3}; memcpy(ix, ixs, 6);
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&q4[0][0], 8);
    p_draw_idx *di = rec(OP_DRAW_IDX, sizeof *di); di->prim = NVN_DRAW_PRIMITIVE_TRIANGLES; di->itype = 0; di->count = 6; di->base_vertex = 4; di->instances = 1; di->ibuf = (uint64_t)(uintptr_t)ix;
    t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    CHECK(all(out, 32, 0, 0, 32, 32, 0xffff0000u), "indexed draw, 8-bit indices, base vertex 4 (%#x)", px(out, 32, 16, 16));
    // culling: a counter-clockwise triangle (GL sense) is front facing with front = CCW; culling the back keeps it, culling the front drops it
    float tri[3][8] = {{-1, -1, 0, 1, 1, 1, 0, 1}, {1, -1, 0, 1, 1, 1, 0, 1}, {-1, 1, 0, 1, 1, 1, 0, 1}};
    poly_st ps_ = {NVN_FACE_BACK, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&tri[0][0], 3); state(4, &ps_, sizeof ps_); draw(NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    int kept = px(out, 32, 4, 4) == 0xff00ffffu;
    ps_.cull = NVN_FACE_FRONT;
    set_rt(&rt); clear(0, 0, 0, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&tri[0][0], 3); state(4, &ps_, sizeof ps_); draw(NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3); t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    CHECK(kept && px(out, 32, 4, 4) == 0xff000000u, "culling: a CCW triangle is front facing (kept with back culling, dropped with front culling)");
    // depth: GL depth (the vertex shader's GL variant), LESS; red at z 0.5, then green behind at 0.8 (hidden), then blue in front at -0.5
    tex_t dz = mk(0x34, 32, 32, NVN_TEXTURE_TARGET_2D, 1);
    p_set_rt *r = rec(OP_SET_RT, sizeof *r); r->n = 1; r->color[0] = &rt; r->depth = &dz;
    clear(0, 0, 0, 1); p_clear_ds *cd = rec(OP_CLEAR_DS, sizeof *cd); cd->depth = 1; cd->depth_mask = 1;
    ds_st ds = {1, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1}; state(5, &ds, sizeof ds);
    float q5[12][8] = {QUAD(-1, -1, 1, 1, 0.5f, 1, 0, 0, 1), QUAD(-1, -1, 1, 1, 0.8f, 0, 1, 0, 1), QUAD(-1, -1, 0, 0, -0.5f, 0, 0, 1, 1)};
    viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); vertices(&q5[0][0], 12); draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 12);
    t2b(&rt, out, 0, 0, 0, 32, 32, 1); float dv[32 * 32]; t2b(&dz, dv, 0, 0, 0, 32, 32, 1); run();
    CHECK(px(out, 32, 24, 24) == 0xff0000ffu && px(out, 32, 4, 4) == 0xffff0000u, "depth test LESS: red over the green behind it, blue in front (%#x %#x)", px(out, 32, 24, 24), px(out, 32, 4, 4));
    CHECK(dv[24 * 32 + 24] > 0.749f && dv[24 * 32 + 24] < 0.751f && dv[4 * 32 + 4] > 0.249f && dv[4 * 32 + 4] < 0.251f, "GL depth convention: z 0.5 → 0.75, z -0.5 → 0.25 (%g %g)", dv[24 * 32 + 24], dv[4 * 32 + 4]);
    // blending: source alpha over a blue target
    blend_st bl = {0, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0};
    color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    float q6[4][8] = {QUAD(-1, -1, 1, 1, 0, 1, 0, 0, 0.5f)};
    set_rt(&rt); clear(0, 0, 1, 1); viewport(0, 0, 32, 32); bind_prog(&g_pr[0], 3); state(0, &bl, sizeof bl); state(1, &cs, sizeof cs); vertices(&q6[0][0], 4); draw(NVN_DRAW_PRIMITIVE_QUADS, 0, 4);
    t2b(&rt, out, 0, 0, 0, 32, 32, 1); run();
    c = px(out, 32, 16, 16);
    CHECK((c & 0xff) >= 127 && (c & 0xff) <= 128 && (c >> 8 & 0xff) == 0 && (c >> 16 & 0xff) >= 127 && (c >> 16 & 0xff) <= 128, "alpha blending: half red over blue (%#x)", c);
    // compute: storage buffer 0 of the compute stage written by 4 invocations, back in guest memory after the commit
    uint32_t *sb = gmem(64);
    bind_prog(&g_pr[3], 0x20); p_bind_buf *s0 = rec(OP_BIND_SSBO, sizeof *s0); *s0 = (p_bind_buf){5, 0, (uint64_t)(uintptr_t)sb, 64};
    p_dispatch *dp = rec(OP_DISPATCH, sizeof *dp); dp->x = 1; dp->y = 1; dp->z = 1; run();
    CHECK(sb[0] == 1 && sb[1] == 4 && sb[2] == 7 && sb[3] == 10, "dispatch: a storage buffer written by the shader reaches guest memory (%u %u %u %u)", sb[0], sb[1], sb[2], sb[3]);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "nobc")) { setenv("DM_VK_NO_BC", "1", 1); g_nobc = 1; }
    test_clear_scissor(); test_b2t_t2b(); test_packaged_and_present(); test_bc1(); test_events_counters(); test_array_and_depth(); test_draws();
    int ve = atomic_load(&nvn_vk_validation_errors);
    if (getenv("DM_VK_VALIDATE")) CHECK(ve == 0, "no validation errors (%d)", ve);
    printf("%d failed\n", g_fail);
    return g_fail;
}
