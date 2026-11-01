// Draws and dispatches of the Vulkan backend (stage 2), included at the end of nvn_vk.c. The semantics follow the Metal replay (dm/gfx/nvn/nvn_mtl.m: state_op, draw,
// encode_args, dispatch); the shaders are the translator's SPIR-V made for Vulkan by dm/android/tools/nvn_vk_shaders.py, found by their key in $DM_VK_SHADERS.
//   · State: the bind records as nvn_mtl.m's state_op keeps them (programs per stage and compute, uniform / storage buffers, texture / image / sampler handles, vertex
//     attributes / streams / buffers, the six state objects, stencil, viewports, depth range, polygon offset, depth clamp, blend colour, primitive restart, rasterizer discard).
//   · Resources per draw, one descriptor set a stage (vertex 0, pixel 1; compute 0), from the shader's .res lines: binding 0 the support buffer (render scale 1, as Metal);
//     cbuf slot 0 / 2 the driver segments c0 / c2 made for this draw as nvn_mtl.m fills them (storage buffer table, point size and base instance, compute dimensions and
//     uniform table; texture / image / sampler handle tables), slot 1 the shader's own constants, slots 3–16 the game's uniform buffers; storage buffers through the c0
//     table (or a uniform buffer); textures and samplers resolved from their handles through the registered pools (mtl_reg_tex / mtl_reg_samp), unbound ones a zero texture.
//     Buffer contents are taken when the draw is recorded (staging; robust buffer access reads past a binding as 0), so the CPU's later writes do not reach it, as on the GPU;
//     a storage buffer the shader writes goes back to guest memory when the GPU finished (the submission is cut there).
//   · Vertex input from the shader's inputs: each NVN attribute it reads, from its stream, in a format of the shader's component type (as nvn_mtl.m vfmt_for_ct); unset ones
//     read zeros. Index buffers: 8-bit indices widened; fans, quads, quad strips, polygons and line loops expanded to lists / strips. Pipelines cached by their state;
//     render passes over the bound targets open at the first draw and end at the next target change or transfer.
//   · Coordinates: the GL depth variant of the vertex shader unless the game set near = 0; lower-left origin = a positive viewport, upper-left = a negative one, the winding
//     as Metal's (both evaluate it in framebuffer space).
// Not yet: storage images (a draw or dispatch that has one is skipped, logged), geometry / tessellation, transform feedback, render conditions, logic ops, advanced blend.

// ---------- Draw state (nvn_mtl.m S) ----------
static struct {
    const prog_t *bst[5], *cprog;
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][256], img[6][32], samp[6][128];
    uint32_t attr[32]; int nattr; vs_st stream[32]; int nstream; NVNbufferRange vbuf[32];
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint8_t st_vmask[2], st_wmask[2], st_ref[2];
    float vp[16][4]; int has_vp; float dr[2]; p_poly_offset po; int depth_clamp; float blend_color[4]; p_prim_restart restart; int rast_discard; uint8_t vp_sw[16][4];
    int tex_base, samp_base;
} D;
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c
static void draw_state_reset(void) {   // nvn_mtl.m state_reset: NVN's (GL's) defaults
    memset(&D, 0, sizeof D);
    for (int i = 0; i < 8; i++) D.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0};
    D.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; D.chmask.mask = 0xffffffffu;
    D.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    D.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    D.st_vmask[0] = D.st_vmask[1] = D.st_wmask[0] = D.st_wmask[1] = 0xff; D.dr[1] = 1;
}
static void bh_fresh(const p_bind_handles *b) { if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n); }
static int state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SET_TEX_POOL: D.tex_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 1); break;
        case OP_SET_SAMP_POOL: D.samp_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 0); break;
        case OP_BIND_PROGRAM: { const p_bind_program *b = a;
            if (b->stages & 0x20) D.cprog = b->prog;
            for (int st = 0; st < 5; st++) if (b->stages >> st & 1) D.bst[st] = b->prog;
            break; }
        case OP_BIND_STATE: { const p_bind_state *b = a; blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); D.blend[bs.target] = bs; break;
                case 1: memcpy(&D.color, b->bytes, sizeof D.color); break;
                case 2: memcpy(&D.chmask, b->bytes, sizeof D.chmask); break;
                case 3: memcpy(&D.ms, b->bytes, sizeof D.ms); break;
                case 4: memcpy(&D.poly, b->bytes, sizeof D.poly); break;
                case 5: memcpy(&D.ds, b->bytes, sizeof D.ds); break;
            }
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->first + b->n <= 32) { memcpy(&D.attr[b->first], b->st, 4 * (size_t)b->n); if (b->first + b->n > D.nattr) D.nattr = b->first + b->n; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->first + b->n <= 32) { memcpy(&D.stream[b->first], b->st, 8 * (size_t)b->n); if (b->first + b->n > D.nstream) D.nstream = b->first + b->n; } break; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); if (b->first + b->n <= 32) memcpy(&D.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage < 6 && b->index < 16) D.ubo[b->stage][b->index] = *b; break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) D.ssbo[b->stage][b->index] = *b; break; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32) break;   // overwritten after it was recorded (nvn_mtl.m)
            bh_fresh(b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 256) memcpy(&D.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&D.samp[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; bh_fresh(b); if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&D.img[b->stage][b->first], b->h, 8 * (size_t)b->n); break; }
        case OP_STENCIL: { const p_stencil *b = a;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) D.st_vmask[f] = (uint8_t)b->value_mask; else if (b->which == 1) D.st_wmask[f] = (uint8_t)b->mask; else D.st_ref[f] = (uint8_t)b->ref; }
            break; }
        case OP_POLY_OFFSET: D.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(D.vp[b->first + i], b->v[i], 16); D.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; D.dr[0] = b->n; D.dr[1] = b->f; break; }
        case OP_DEPTH_CLAMP: D.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_VP_SWIZZLE: { const p_vp_swizzle *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(D.vp_sw[b->first], b->sw, 4 * (size_t)b->n); break; }
        case OP_BLEND_COLOR: memcpy(D.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_PRIM_RESTART: D.restart = *(const p_prim_restart *)a; break;
        case OP_RAST_DISCARD: D.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_RENDER_COND: case OP_TF_BIND: case OP_TF_STATE: { static int n; if (!n++) VLOG("[RED] render conditions and transform feedback are not executed yet on Vulkan (first op %u)", op); break; }
        default: return 0;
    }
    return 1;
}

// ---------- Registered textures and samplers (the descriptor pools' host table: hle_nvn.c dpool_add) ----------
typedef struct { tex_t t; view_t v; int has, used; } treg;
static treg *g_treg; static int g_ntreg, g_captreg, g_tcap = 1 << 30;
static samp_par *g_sreg; static uint8_t *g_sused; static int g_nsreg, g_capsreg;
static void treg_grow(int id) { if (id < g_captreg) return; int n = g_captreg ? g_captreg : 1024; while (n <= id) n *= 2; g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_captreg, 0, (size_t)(n - g_captreg) * sizeof *g_treg); g_captreg = n; }
void mtl_set_tex_pool_n(int n) { lock(); g_tcap = n > 0 ? n : 1 << 30; unlock(); }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || !t) return;
    lock(); treg_grow(id); g_treg[id].t = *t; g_treg[id].has = v != NULL; if (v) g_treg[id].v = *v; g_treg[id].used = 1; if (id >= g_ntreg) g_ntreg = id + 1; unlock();
}
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { mtl_reg_tex(id, t, v); }
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || !p) return;
    lock();
    if (id >= g_capsreg) { int n = g_capsreg ? g_capsreg : 256; while (n <= id) n *= 2; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); g_sused = realloc(g_sused, (size_t)n); memset(g_sused + g_capsreg, 0, (size_t)(n - g_capsreg)); g_capsreg = n; }
    g_sreg[id] = *p; g_sused[id] = 1; if (id >= g_nsreg) g_nsreg = id + 1;
    unlock();
}
void mtl_prog_release(prog_t *p) { (void)p; }

// ---------- Shaders ----------
enum { R_SUPPORT, R_CBUF, R_SBUF, R_TEX, R_IMG, R_UNKNOWN };
typedef struct { int b, dtype, count, role, a0, a1, a2; } vbind;
typedef struct vshader {
    int stage; char key[17];
    VkShaderModule mod, mod_gl;
    vbind *b; int nb; int has_img;
    uint32_t in_mask; uint8_t in_kind[32], in_n[32]; uint32_t out_mask; uint8_t out_kind[8];
    uint32_t local[3]; uint8_t *c1; size_t c1n;
    VkDescriptorSetLayout dsl;
} vshader;
static nvn_hmap g_shmap = {.klen = 16};    // key (16 hex digits) → stage + 1
static nvn_hmap g_shaders = {.klen = 16};  // key → vshader (or (void *)1: missing)
static int g_shmap_loaded;
static void shmap_load(void) {
    if (g_shmap_loaded) return; g_shmap_loaded = 1;
    const char *p = getenv("DM_NVN_SHMAP"); FILE *f = p ? fopen(p, "r") : NULL;
    if (!f) { VLOG("[RED] no shader map (DM_NVN_SHMAP=%s): nothing is drawn", p ? p : "unset"); return; }
    char line[512]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !strncmp(line, "package", 7)) continue;
        char *col[5] = {0}; int c = 0; char *s = line;
        for (char *t = strsep(&s, "\t\n"); t && c < 5; t = strsep(&s, "\t\n")) col[c++] = t;
        if (c < 4 || strlen(col[3]) != 16) continue;
        static const char *names[6] = {"vertex", "pixel", "geometry", "hull", "domain", "compute"};
        for (int st = 0; st < 6; st++) if (!strcmp(col[2], names[st])) { nvn_hm_put(&g_shmap, col[3], (void *)(intptr_t)(st + 1)); n++; }
    }
    fclose(f); VLOG("shader map: %d shaders", n);
}
static uint8_t *slurp_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(sz > 0 ? (size_t)sz : 1); if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz; return p;
}
static VkShaderModule module_load(const char *path) {
    size_t n; uint8_t *w = slurp_file(path, &n); if (!w) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, (const uint32_t *)w};
    VkShaderModule m = VK_NULL_HANDLE; VkResult r = vkCreateShaderModule(g_dev, &ci, NULL, &m); free(w);
    if (r < 0) { VLOG("[RED] shader %s: vkCreateShaderModule %d", path, (int)r); return VK_NULL_HANDLE; }
    return m;
}
static vshader *shader_load(const char *key, int stage) {
    void *have = nvn_hm_get(&g_shaders, key); if (have) return have == (void *)1 ? NULL : have;
    const char *dir = getenv("DM_VK_SHADERS"); char p[1024];
    vshader *s = calloc(1, sizeof *s); s->stage = stage; memcpy(s->key, key, 16);
    snprintf(p, sizeof p, "%s/%s.res", dir ? dir : ".", key);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            vbind b = {0}; char role[16] = ""; int loc, n; char k;
            if (sscanf(line, "b %d %d %d %15s %d %d %d", &b.b, &b.dtype, &b.count, role, &b.a0, &b.a1, &b.a2) >= 4) {
                b.role = !strcmp(role, "support") ? R_SUPPORT : !strcmp(role, "cbuf") ? R_CBUF : !strcmp(role, "sbuf") ? R_SBUF : !strcmp(role, "tex") ? R_TEX : !strcmp(role, "img") ? R_IMG : R_UNKNOWN;
                if (b.role == R_IMG || b.dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) s->has_img = 1;
                s->b = realloc(s->b, (size_t)(s->nb + 1) * sizeof *s->b); s->b[s->nb++] = b;
            } else if (sscanf(line, "in %d %c %d", &loc, &k, &n) == 3 && loc >= 0 && loc < 32) { s->in_mask |= 1u << loc; s->in_kind[loc] = (uint8_t)k; s->in_n[loc] = (uint8_t)n; }
            else if (sscanf(line, "out %d %c", &loc, &k) == 2 && loc >= 0 && loc < 8) { s->out_mask |= 1u << loc; s->out_kind[loc] = (uint8_t)k; }
            else if (sscanf(line, "local %u %u %u", &s->local[0], &s->local[1], &s->local[2]) == 3) {}
        }
        fclose(f);
        snprintf(p, sizeof p, "%s/%s.spv", dir ? dir : ".", key); s->mod = module_load(p);
        if (stage == 0) { snprintf(p, sizeof p, "%s/%s.gl.spv", dir ? dir : ".", key); s->mod_gl = module_load(p); }
        snprintf(p, sizeof p, "%s/%s.c1", dir ? dir : ".", key); s->c1 = slurp_file(p, &s->c1n);
    }
    if (!s->mod) { static int n; if (n++ < 16) VLOG("[RED] shader %s (stage %d) not found in %s: its draws are skipped", key, stage, dir ? dir : "(DM_VK_SHADERS unset)"); free(s->b); free(s); nvn_hm_put(&g_shaders, key, (void *)1); return NULL; }
    VkDescriptorSetLayoutBinding *lb = calloc((size_t)s->nb + 1, sizeof *lb);
    VkShaderStageFlags sf = stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    for (int i = 0; i < s->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){(uint32_t)s->b[i].b, (VkDescriptorType)s->b[i].dtype, (uint32_t)s->b[i].count, sf, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)s->nb, lb};
    VKC(vkCreateDescriptorSetLayout(g_dev, &ci, NULL, &s->dsl)); free(lb);
    nvn_hm_put(&g_shaders, key, s);
    return s;
}
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]);   // hle_nvn.c: the shader key of a program's stage index
static vshader *shader_of(const prog_t *p, int stage) {   // the program's shader for an NVN stage
    if (!p) return NULL;
    shmap_load();
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t md[8]; if (prog_key(p, i, md)) continue;
        char k[17]; for (int j = 0; j < 8; j++) snprintf(k + 2 * j, 3, "%02x", md[j]);
        intptr_t st = (intptr_t)nvn_hm_get(&g_shmap, k);
        if (st == stage + 1) return shader_load(k, stage);
    }
    return NULL;
}

// ---------- Descriptor pools (one chain per submission) and per-submission snapshots of guest memory ----------
static VkDescriptorPool g_dpool[64]; static int g_ndpool, g_dcur;
static VkDescriptorSet desc_alloc(VkDescriptorSetLayout l) {
    for (;;) {
        if (g_dcur == g_ndpool) {
            if (g_ndpool == 64) VDIE("descriptor pools exhausted in one submission");
            VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 512},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
            VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, sizeof sz / sizeof *sz, sz};
            VKC(vkCreateDescriptorPool(g_dev, &ci, NULL, &g_dpool[g_ndpool++]));
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool[g_dcur], 1, &l};
        VkDescriptorSet ds; VkResult r = vkAllocateDescriptorSets(g_dev, &ai, &ds);
        if (r == VK_SUCCESS) return ds;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) VDIE("vkAllocateDescriptorSets: %d", (int)r);
        g_dcur++;
    }
}
static void desc_reset(void) { for (int i = 0; i < g_ndpool; i++) vkResetDescriptorPool(g_dev, g_dpool[i], 0); g_dcur = 0; }
typedef struct { uint64_t addr, size; } snapkey;
typedef struct { stage_t s; } snapval;
static nvn_hmap g_snap = {.klen = sizeof(snapkey)};
static void snap_free(void *p) { free(p); }
static stage_t g_zero;
static void stage_cache_clear(void) { nvn_hm_clear(&g_snap, snap_free); g_zero.p = NULL; }   // the zero buffer too: staging may be reused   // 64 KiB of zeros (unbound buffers), made once per submission
static stage_t zeros(void) { if (!g_zero.p) { g_zero = stage_al(65536, 256); memset(g_zero.p, 0, 65536); } return g_zero; }
// A copy of guest memory [addr, addr + size) as the draw being recorded sees it; addresses outside the game's memory pools read as zeros
static stage_t snap(uint64_t addr, uint64_t size, VkDeviceSize align) {
    if (!size) size = 16;
    snapkey k = {addr, size}; snapval *v = nvn_hm_get(&g_snap, &k);
    if (v && !(v->s.off & (align - 1))) return v->s;
    stage_t s = stage_al(size, align);
    void *buf; uint64_t off;
    if (addr && mtl_addr(addr, size, &buf, &off)) memcpy(s.p, (const void *)(uintptr_t)addr, size);
    else memset(s.p, 0, size);
    if (!v) { v = malloc(sizeof *v); nvn_hm_put(&g_snap, &k, v); }
    v->s = s; return s;
}

// ---------- Driver segments c0 / c2 (nvn_mtl.m encode_args) ----------
enum { C0_PSIZE = 0x610, C0_BASE_INST = 0x644, C0_DIMS = 0x200, C0_CUBO = 0x210, DRV = 0xA00 };
enum { C2T = 0x20, C2T_SLOTS = 41, C2I = 0x120, C2I_SLOTS = 8, C2S = 0x168, C2SAMP = 0x568 };
static int c0_base(int st) { return st == 0 ? 0x110 : st == 1 ? 0x510 : st == 2 ? 0x410 : st == 5 ? 0x310 : -1; }
static void c0_make(int st, uint8_t *c, uint32_t base_instance, const uint32_t dims[3]) {
    memset(c, 0, DRV);
    int b = c0_base(st);
    for (int r = 0; r < 16 && b >= 0; r++) { uint64_t a = D.ssbo[st][r].addr; uint32_t n = (uint32_t)D.ssbo[st][r].size; memcpy(c + b + 16 * r, &a, 8); memcpy(c + b + 16 * r + 8, &n, 4); }
    if (st != 5) { float ps = 2047.0f; memcpy(c + C0_PSIZE, &ps, 4); memcpy(c + C0_BASE_INST, &base_instance, 4); }
    else {
        if (dims) memcpy(c + C0_DIMS, dims, 12);
        for (int r = 0; r < 16; r++) { uint64_t a = D.ubo[5][r].addr; uint32_t n = (uint32_t)D.ubo[5][r].size; memcpy(c + C0_CUBO + 16 * r, &a, 8); memcpy(c + C0_CUBO + 16 * r + 8, &n, 4); }
    }
}
static uint64_t tex_slot(int st, unsigned r) {   // nvn_mtl.m tex_slot_h: a combined slot past 31 shares its word with an image slot, and a bound image is what lies there
    return r >= 32 && r - 32 < C2I_SLOTS && D.img[st][r - 32] ? D.img[st][r - 32] : r < 256 ? D.tex[st][r] : 0;
}
static void c2_make(int st, uint8_t *c) {
    memset(c, 0, DRV);
    for (int r = 0; r < C2I_SLOTS; r++) { uint32_t v = (uint32_t)D.img[st][r]; memcpy(c + C2I + 8 * r, &v, 4); }
    for (int r = 0; r < C2T_SLOTS; r++) {
        if (r >= 32 && (!D.tex[st][r] || D.img[st][r - 32])) continue;
        uint32_t v = (uint32_t)D.tex[st][r]; memcpy(c + C2T + 8 * r, &v, 4);
    }
    for (int r = 0; r < 128; r++) { uint32_t v = (uint32_t)D.tex[st][128 + r]; memcpy(c + C2S + 8 * r, &v, 4); v = (uint32_t)D.samp[st][r]; memcpy(c + C2SAMP + 8 * r, &v, 4); }
}

// ---------- Textures and samplers of a binding ----------
static VkImageViewType want_view(int type) {   // Ryujinx's SamplerType: 1 1D, 3 2D, 4 3D, 5 cube; Array 0x100; Multisample 0x200
    int base = type & 0xff, arr = (type & 0x100) != 0;
    switch (base) {
        case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 4: return VK_IMAGE_VIEW_TYPE_3D;
        case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
static VkImageView g_dummy[8][2]; static VkImage g_dummy_img[8][2]; static vmem g_dummy_m[8][2];   // [view type][depth]: 1x1 zero (depth: 1.0) textures for unbound slots
static VkImageView dummy_view(VkImageViewType vt, int depth) {
    if (g_dummy[vt][depth]) return g_dummy[vt][depth];
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageType it = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    VkFormat f = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM; uint32_t layers = cube ? 6 : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, it, f, {1, 1, 1}, 1, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(vkCreateImage(g_dev, &ci, NULL, &g_dummy_img[vt][depth])); VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, g_dummy_img[vt][depth], &rq);
    g_dummy_m[vt][depth] = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); VKC(vkBindImageMemory(g_dev, g_dummy_img[vt][depth], g_dummy_m[vt][depth].mem, g_dummy_m[vt][depth].off));
    rec_begin(); pass_end();
    VkImageAspectFlags a = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                               g_dummy_img[vt][depth], {a, 0, 1, 0, layers}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkImageSubresourceRange rg = {a, 0, 1, 0, layers};
    if (depth) { VkClearDepthStencilValue z = {1, 0}; vkCmdClearDepthStencilImage(g_cb, g_dummy_img[vt][depth], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    else { VkClearColorValue z = {{0}}; vkCmdClearColorImage(g_cb, g_dummy_img[vt][depth], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    barrier();
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dummy_img[vt][depth], vt, f, {0}, {a, 0, 1, 0, layers}};
    VKC(vkCreateImageView(g_dev, &vi, NULL, &g_dummy[vt][depth]));
    return g_dummy[vt][depth];
}
static const samp_par g_samp_default = {NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0, 1000, 0, 1, {0}};
// A c2 word offset (in u32 words) → what the handle there names: kind 0 combined slot r, 1 separate texture slot, 2 separate sampler slot, 3 image slot; -1 none
static int c2_word(unsigned word, int *slot) {
    unsigned b = word * 4;
    if (b >= C2SAMP && (b - C2SAMP) % 8 == 0 && (b - C2SAMP) / 8 < 128) { *slot = (int)((b - C2SAMP) / 8); return 2; }
    if (b >= C2S && b < C2SAMP && (b - C2S) % 8 == 0) { *slot = (int)((b - C2S) / 8); return 1; }
    if (b >= C2I && b < C2I + 8 * C2I_SLOTS && (b - C2I) % 8 == 0) { *slot = (int)((b - C2I) / 8); return 3; }
    if (b >= C2T && b < C2S && (b - C2T) % 8 == 0) { *slot = (int)((b - C2T) / 8); return 0; }
    return -1;
}
// The texture id and sampler id a texture binding reads, element k of it (nvn_mtl.m encode_args TEX / SAMP / SAMPT / TEXU / TEXA)
static void tex_ids(int st, const vbind *b, int k, int *tid, int *sid) {
    uint32_t h = (uint32_t)b->a1; int cb = b->a2; *tid = *sid = -1;
    unsigned w0 = h & 0x3fff, w1 = (h >> 14) & 0x3fff, type = h >> 28;
    if (cb == 2) {
        int s0, s1, k0 = c2_word(w0 + 2 * (unsigned)k, &s0), k1 = w1 ? c2_word(w1, &s1) : -1;
        uint32_t comb = 0;
        if (k0 == 0) { comb = (uint32_t)tex_slot(st, (unsigned)s0); *tid = (int)(comb & 0xfffff); *sid = (int)(comb >> 20); }
        else if (k0 == 1) *tid = (int)(D.tex[st][128 + s0] & 0xfffff);
        else if (k0 == 3) *tid = (int)(D.img[st][s0] & 0xfffff);
        if (k1 == 2) *sid = (int)((uint32_t)D.samp[st][s1] >> 20);
        else if (k1 == 0) *sid = (int)((uint32_t)tex_slot(st, (unsigned)s1) >> 20);
        if (type == 3) *sid = (int)w1;   // a constant sampler id
    } else if (cb >= 3 && cb - 3 < 16 && D.ubo[st][cb - 3].addr) {   // a handle in a game uniform buffer, with the bound pools' bases
        uint64_t a = D.ubo[st][cb - 3].addr + 4 * (uint64_t)w0 + 8 * (uint64_t)k; void *buf; uint64_t off; uint32_t v = 0;
        if (mtl_addr(a, 4, &buf, &off)) memcpy(&v, (const void *)(uintptr_t)a, 4);
        if (v) { *tid = (int)(v & 0xfffff) + D.tex_base; *sid = (int)(v >> 20) + D.samp_base; }
    }
}
static void tex_descriptor(int st, const vbind *b, int k, VkDescriptorImageInfo *ii) {
    int tid, sid; tex_ids(st, b, k, &tid, &sid);
    int shadow = (b->a0 & 0x400) != 0; VkImageViewType vt = want_view(b->a0);
    VkImageView iv = VK_NULL_HANDLE;
    if (tid > 0 && tid < g_ntreg && tid < g_tcap && g_treg[tid].used) {
        vtex *v = tex_use(&g_treg[tid].t, 0);
        if (v && !(v->samples > 1) == !(b->a0 & 0x200)) iv = sampled_view(v, &g_treg[tid].v, g_treg[tid].has, (int)vt);
    }
    if (!iv) iv = dummy_view(vt, shadow);
    const samp_par *sp = sid >= 0 && sid < g_nsreg && g_sused[sid] ? &g_sreg[sid] : &g_samp_default;
    *ii = (VkDescriptorImageInfo){sampler_get(sp, shadow), iv, VK_IMAGE_LAYOUT_GENERAL};
}

// ---------- One stage's descriptor set ----------
typedef struct { uint64_t addr; stage_t s; uint64_t size; } wback;   // a storage buffer the shader writes: back to guest memory after the GPU
static wback g_wb[64]; static int g_nwb;
static VkDescriptorSet stage_set(int st, const vshader *s, uint32_t base_instance, const uint32_t dims[3]) {
    VkDescriptorSet ds = desc_alloc(s->dsl);
    VkWriteDescriptorSet *w = calloc((size_t)s->nb, sizeof *w);
    VkDescriptorBufferInfo *bi = calloc((size_t)s->nb, sizeof *bi);
    VkDescriptorImageInfo *ii = calloc((size_t)s->nb * 32 + 1, sizeof *ii); int nii = 0;
    VkDeviceSize ua = g_props.limits.minUniformBufferOffsetAlignment, sa = g_props.limits.minStorageBufferOffsetAlignment;
    int nw = 0; uint8_t c0[DRV], c2[DRV]; int have0 = 0, have2 = 0;
    for (int i = 0; i < s->nb; i++) {
        const vbind *b = &s->b[i];
        VkWriteDescriptorSet *x = &w[nw]; *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, (uint32_t)b->b, 0, 1, (VkDescriptorType)b->dtype, NULL, &bi[i], NULL};
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            stage_t t; uint64_t n = 16;
            if (b->role == R_SUPPORT) {   // render scale 1 for every target (FragCoord is divided by it); the rest 0, as Metal
                n = 1392; t = stage_al(n, ua); memset(t.p, 0, n); for (int r = 0; r < 73; r++) { float one = 1; memcpy(t.p + 192 + 16 * r, &one, 4); }
            } else if (b->role == R_CBUF && b->a0 == 0) { if (!have0) { c0_make(st, c0, base_instance, dims); have0 = 1; } n = DRV; t = stage_al(n, ua); memcpy(t.p, c0, n); }
            else if (b->role == R_CBUF && b->a0 == 2) { if (!have2) { c2_make(st, c2); have2 = 1; } n = DRV; t = stage_al(n, ua); memcpy(t.p, c2, n); }
            else if (b->role == R_CBUF && b->a0 == 1) { n = s->c1n ? (s->c1n + 15) & ~(size_t)15 : 16; t = stage_al(n, ua); memset(t.p, 0, n); if (s->c1n) memcpy(t.p, s->c1, s->c1n); }
            else if (b->role == R_CBUF && b->a0 >= 3 && b->a0 < 19) {
                const p_bind_buf *u = &D.ubo[st][b->a0 - 3];
                uint64_t lim = g_props.limits.maxUniformBufferRange < 65536 ? g_props.limits.maxUniformBufferRange : 65536;
                if (u->addr) { n = u->size > lim ? lim : u->size ? u->size : 16; t = snap(u->addr, n, ua); } else { t = zeros(); n = lim; }
            } else if (b->role == R_SBUF) {
                uint64_t addr = 0, size = 0; int cbs = b->a0; unsigned off = (unsigned)b->a1 * 4;
                if (cbs == 0) {
                    if (st == 5 && off >= C0_CUBO && off < C0_CUBO + 0x100) { const p_bind_buf *u = &D.ubo[5][(off - C0_CUBO) / 16]; addr = u->addr; size = u->size; }
                    else { int base = c0_base(st); if (base >= 0 && off >= (unsigned)base && (off - (unsigned)base) / 16 < 16) { const p_bind_buf *u = &D.ssbo[st][(off - (unsigned)base) / 16]; addr = u->addr; size = u->size; } }
                } else if (cbs >= 3 && cbs - 3 < 16 && D.ubo[st][cbs - 3].addr) {   // the address in a game uniform buffer
                    uint64_t a = D.ubo[st][cbs - 3].addr + off; void *bf; uint64_t of;
                    if (mtl_addr(a, 12, &bf, &of)) { memcpy(&addr, (const void *)(uintptr_t)a, 8); uint32_t z; memcpy(&z, (const void *)(uintptr_t)(a + 8), 4); size = z; }
                }
                if (addr && size) {
                    t = snap(addr, size, sa); n = size;
                    if (b->a2 & 1 && g_nwb < 64) {   // written by the shader: a copy of its own (not shared with other draws' snapshots), back after the GPU
                        stage_t own = stage_al(size, sa); memcpy(own.p, t.p, size); t = own; g_wb[g_nwb++] = (wback){addr, own, size};
                    }
                } else { t = zeros(); n = 65536; }
            } else { t = zeros(); n = 16; }
            bi[i] = (VkDescriptorBufferInfo){t.buf, t.off, n};
        } else if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
            int cnt = b->count > 0 ? b->count : 1; x->descriptorCount = (uint32_t)cnt; x->pBufferInfo = NULL; x->pImageInfo = &ii[nii];
            for (int k = 0; k < cnt; k++) tex_descriptor(st, b, k, &ii[nii++]);
        } else continue;   // texel buffers and storage images: not yet (draws with storage images were skipped before this)
        nw++;
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)nw, w, 0, NULL);
    free(w); free(bi); free(ii);
    return ds;
}

// ---------- Render passes over the bound targets ----------
static int g_pass; static VkRenderPass g_prp; static VkExtent2D g_pext; static uint32_t g_pfmt_n; static VkFormat g_pfmt[9]; static VkSampleCountFlagBits g_psamples; static int g_pdepth;
static void pass_end(void) { if (g_pass) { vkCmdEndRenderPass(g_cb); g_pass = 0; barrier(); } }
typedef struct { uint32_t n; VkFormat f[9]; int32_t depth, samples; } rpkey;
static nvn_hmap g_rpd = {.klen = sizeof(rpkey)};
static VkRenderPass draw_rp(const rpkey *k) {
    VkRenderPass rp = (VkRenderPass)nvn_hm_get(&g_rpd, k); if (rp) return rp;
    VkAttachmentDescription ad[9]; VkAttachmentReference cr[8], dr = {0, VK_IMAGE_LAYOUT_GENERAL}; uint32_t na = 0, nc = 0;
    for (uint32_t i = 0; i < k->n; i++) {
        if (k->f[i]) { ad[na] = (VkAttachmentDescription){0, k->f[i], (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL}; cr[nc++] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL}; }
        else cr[nc++] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (k->depth) { ad[na] = (VkAttachmentDescription){0, k->f[8], (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL}; dr.attachment = na++; }
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, nc, cr, NULL, k->depth ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, na, ad, 1, &sd, 0, NULL};
    VKC(vkCreateRenderPass(g_dev, &ci, NULL, &rp)); nvn_hm_put(&g_rpd, k, (void *)rp); return rp;
}
static rpkey g_prk;
static int pass_begin(void) {   // the render pass of the bound targets (open already for consecutive draws); 0 when there is nothing to draw into
    if (g_pass) return 1;
    rpkey k; memset(&k, 0, sizeof k); VkImageView views[9]; uint32_t nv = 0, w = 1u << 30, h = 1u << 30, layers = 1u << 30; int samples = 0;
    int n = S.has_rt ? S.rt.n : 0; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        vtex *v = S.rt.color[i] ? tex_use(S.rt.color[i], 1) : NULL;
        if (!v || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { k.f[i] = 0; continue; }
        unsigned lv, la, nl; view_range(v, &S.rt.cview[i], S.rt.has_cview[i], &lv, &la, &nl);
        uint32_t lw, lh, ld; lvl_dims(v, lv, &lw, &lh, &ld); if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
        k.f[i] = v->fmt; samples = v->samples; views[nv++] = view_make(v, lv, la, nl, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    k.n = (uint32_t)n;
    if (S.has_rt && S.rt.depth) {
        vtex *v = tex_use(S.rt.depth, 1);
        if (v && v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            unsigned lv, la, nl; view_range(v, &S.rt.dview, S.rt.has_dview, &lv, &la, &nl);
            uint32_t lw, lh, ld; lvl_dims(v, lv, &lw, &lh, &ld); if (lw < w) w = lw; if (lh < h) h = lh; if (nl < layers) layers = nl;
            k.depth = 1; k.f[8] = v->fmt; if (!samples) samples = v->samples; views[nv++] = view_make(v, lv, la, nl, v->aspect);
        }
    }
    if (!nv) {   // no targets: sized by viewport 0 (Vulkan 1.1 takes an attachment-less framebuffer)
        w = D.has_vp && D.vp[0][2] > 0 ? (uint32_t)D.vp[0][2] : 1; h = D.has_vp && D.vp[0][3] > 0 ? (uint32_t)D.vp[0][3] : 1; layers = 1; samples = 1;
    }
    k.samples = samples ? samples : 1; if (layers > 2048) layers = 1;
    VkRenderPass rp = draw_rp(&k);
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, views, w, h, layers};
    VkFramebuffer fb; VKC(vkCreateFramebuffer(g_dev, &fi, NULL, &fb)); trans_add(1, (uint64_t)fb);
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {w, h}}, 0, NULL};
    vkCmdBeginRenderPass(g_cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_pass = 1; g_prp = rp; g_pext = (VkExtent2D){w, h}; g_prk = k;
    return 1;
}

// ---------- Vertex formats (the shader's component type decides the class, nvn_mtl.m vfmt_for_ct) ----------
static int fmt_kind(VkFormat f) {   // 'f' normalized / float / scaled, 'u' unsigned integer, 'i' signed integer
    VkFormatProperties p; (void)p;
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 'u';
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32: return 'i';
        default: return 'f';
    }
}
static VkFormat vfmt(int nvn, int want) {
    nvn_vkfmt e = nvn_fmt_to_vk(nvn); VkFormat f = e.vk;
    if (e.expand) {   // the 3-component formats are vertex formats in Vulkan
        static const VkFormat three[][4] = {   // by component bytes: unorm/float, snorm, uint, sint
            {0}, {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT},
            {VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT}, {0},
            {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT}};
        int cls = fmt_kind(f) == 'u' ? 2 : fmt_kind(f) == 'i' ? 3 : (f == VK_FORMAT_R8G8B8A8_SNORM || f == VK_FORMAT_R16G16B16A16_SNORM) ? 1 : 0;
        if (f == VK_FORMAT_R16G16B16A16_SFLOAT) f = VK_FORMAT_R16G16B16_SFLOAT; else f = three[e.expand][cls];
    }
    if (!f) return VK_FORMAT_UNDEFINED;
    int k = fmt_kind(f);
    if (k == want || !want) return f;
    // another class: 32-bit components keep their bits (float ↔ integer), narrower integers convert (scaled), float data read as integers reads the integer of the same width
    static const struct { VkFormat fl, u, i, us, ss; } map[] = {
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, 0, 0}, {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, 0, 0},
        {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT, 0, 0}, {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, 0, 0},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED}, {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED}, {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED},
        {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED},
        {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED},
        {VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        int is32 = !map[i].us;
        int hit = f == map[i].fl || f == map[i].u || f == map[i].i || (!is32 && (f == map[i].us || f == map[i].ss || f == map[i].fl + 1));   // a narrow row: its SNORM is UNORM + 1
        if (!hit) continue;
        if (want == 'u') return map[i].u;
        if (want == 'i') return map[i].i;
        return is32 ? map[i].fl : (k == 'i' ? map[i].ss : map[i].us);
    }
    return f;
}
static uint32_t vfmt_size(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_USCALED: case VK_FORMAT_R8_SSCALED: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_USCALED: case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_USCALED: case VK_FORMAT_R16_SSCALED: return 2;
        case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8_USCALED: case VK_FORMAT_R8G8B8_SSCALED: return 3;
        case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_USCALED: case VK_FORMAT_R16G16B16_SSCALED: return 6;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_USCALED: case VK_FORMAT_R16G16B16A16_SSCALED: return 8;
        case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
        default: return 4;
    }
}

// ---------- Pipelines ----------
typedef struct {
    uint64_t vs, ps, rp; uint8_t topo, restart, cull, front, pmode, dbias, dclamp, discard, samples, a2c, dt, dw, df, st;
    uint8_t sf[2], sop[2][3], ncol, nattr, nbind, gl; uint32_t pad;
    struct { uint32_t loc, binding, fmt, off; } attr[32];
    struct { uint32_t binding, rate, stride, pad; } bind[33];
    struct { uint8_t en, mask, sc, dc, sa, da, oc, oa; } col[8];
} pkey;
static nvn_hmap g_pipe = {.klen = sizeof(pkey)};
static nvn_hmap g_play = {.klen = 16};
static VkPipelineLayout pipe_layout(VkDescriptorSetLayout a, VkDescriptorSetLayout b) {
    uint64_t k[2] = {(uint64_t)a, (uint64_t)b};
    VkPipelineLayout l = (VkPipelineLayout)nvn_hm_get(&g_play, k); if (l) return l;
    static VkDescriptorSetLayout empty;
    if (!empty) { VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL}; VKC(vkCreateDescriptorSetLayout(g_dev, &ci, NULL, &empty)); }
    VkDescriptorSetLayout sets[2] = {a ? a : empty, b ? b : empty};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, b ? 2 : 1, sets, 0, NULL};
    VKC(vkCreatePipelineLayout(g_dev, &ci, NULL, &l)); nvn_hm_put(&g_play, k, (void *)l); return l;
}
static VkBlendFactor bfac(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(int e) { return e == 2 ? VK_BLEND_OP_SUBTRACT : e == 3 ? VK_BLEND_OP_REVERSE_SUBTRACT : e == 4 ? VK_BLEND_OP_MIN : e == 5 ? VK_BLEND_OP_MAX : VK_BLEND_OP_ADD; }
static int dual_src(int f) { return f >= 0x10 && f <= 0x13; }
static VkPipeline pipeline(const pkey *k, VkPipelineLayout lay, const vshader *vs, const vshader *ps) {
    VkPipeline p = (VkPipeline)nvn_hm_get(&g_pipe, k); if (p) return p == (VkPipeline)1 ? VK_NULL_HANDLE : p;
    VkPipelineShaderStageCreateInfo stg[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, k->gl ? vs->mod_gl : vs->mod, "main", NULL},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps ? ps->mod : VK_NULL_HANDLE, "main", NULL}};
    VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDescription vb[33];
    for (int i = 0; i < k->nattr; i++) va[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (int i = 0; i < k->nbind; i++) vb[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, (VkVertexInputRate)k->bind[i].rate};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, k->nbind, vb, k->nattr, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, k->restart};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->dclamp, k->discard, (VkPolygonMode)k->pmode, (VkCullModeFlags)k->cull,
                                                 (VkFrontFace)k->front, k->dbias, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)k->samples, 0, 0, NULL, k->a2c, 0};
    VkStencilOpState so[2];
    for (int f = 0; f < 2; f++) so[f] = (VkStencilOpState){(VkStencilOp)k->sop[f][0], (VkStencilOp)k->sop[f][2], (VkStencilOp)k->sop[f][1], (VkCompareOp)k->sf[f], 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k->dt, k->dw, (VkCompareOp)k->df, 0, k->st, so[0], so[1], 0, 1};
    VkPipelineColorBlendAttachmentState cb[8];
    for (int i = 0; i < k->ncol; i++) cb[i] = (VkPipelineColorBlendAttachmentState){k->col[i].en, (VkBlendFactor)k->col[i].sc, (VkBlendFactor)k->col[i].dc, (VkBlendOp)k->col[i].oc,
                                                                                      (VkBlendFactor)k->col[i].sa, (VkBlendFactor)k->col[i].da, (VkBlendOp)k->col[i].oa, k->col[i].mask};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, VK_LOGIC_OP_COPY, k->ncol, cb, {0, 0, 0, 0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, ps && ps->mod ? 2 : 1, stg, &vi, &ia, NULL, &vp, &rs, &ms, &dsi, &cbs, &dy, lay, (VkRenderPass)k->rp, 0, VK_NULL_HANDLE, -1};
    VkResult r = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r < 0) { static int n; if (n++ < 8) VLOG("[RED] pipeline for shaders %s / %s: vkCreateGraphicsPipelines %d; its draws are skipped", vs->key, ps ? ps->key : "-", (int)r); p = VK_NULL_HANDLE; }
    nvn_hm_put(&g_pipe, k, p ? (void *)p : (void *)1);
    return p;
}

// ---------- Indices: widening and primitive expansion ----------
static int g_red_prim;
// The draw's indices as u32 (expanded for fans, quads, quad strips, polygons and line loops; a restart index starts the primitive again). NULL: no index list needed
static uint32_t *prim_indices(int prim, const uint32_t *src, uint32_t n, int restart, uint32_t ri, uint32_t *out_n, int *topo) {
    *topo = -1;
    uint32_t *o = malloc(((size_t)n * 3 + 8) * sizeof *o); uint32_t m = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i < n && !(restart && src[i] == ri)) continue;
        uint32_t c = i - start; const uint32_t *v = src + start;
        switch (prim) {
            case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: for (uint32_t k = 1; k + 1 < c; k++) { o[m++] = v[0]; o[m++] = v[k]; o[m++] = v[k + 1]; } break;
            case NVN_DRAW_PRIMITIVE_QUADS: for (uint32_t k = 0; k + 3 < c; k += 4) { o[m++] = v[k]; o[m++] = v[k + 1]; o[m++] = v[k + 2]; o[m++] = v[k]; o[m++] = v[k + 2]; o[m++] = v[k + 3]; } break;
            case NVN_DRAW_PRIMITIVE_QUAD_STRIP: for (uint32_t k = 0; k + 3 < c; k += 2) { o[m++] = v[k]; o[m++] = v[k + 1]; o[m++] = v[k + 3]; o[m++] = v[k]; o[m++] = v[k + 3]; o[m++] = v[k + 2]; } break;
            case NVN_DRAW_PRIMITIVE_LINE_LOOP: if (c > 1) { for (uint32_t k = 0; k < c; k++) { o[m++] = v[k]; o[m++] = v[(k + 1) % c]; } } break;
        }
        start = i + 1;
    }
    *topo = prim == NVN_DRAW_PRIMITIVE_LINE_LOOP ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    *out_n = m; return o;
}
static int vk_topo(int prim) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_QUADS: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: case NVN_DRAW_PRIMITIVE_POLYGON: case NVN_DRAW_PRIMITIVE_LINE_LOOP: return -2;   // expanded
        default: return -1;
    }
}

// ---------- A draw ----------
static int is_int_fmt(VkFormat f) { return fmt_kind(f) != 'f'; }
static void wb_finish(void) {   // storage buffers the draw writes: back to guest memory once the GPU ran it
    if (!g_nwb) return;
    pass_end();
    for (int i = 0; i < g_nwb; i++) post_add((post_t){3, NULL, 0, 0, g_wb[i].addr, 0, g_wb[i].s.p, 1, (uint32_t)g_wb[i].size, (uint32_t)g_wb[i].size, 1, g_wb[i].size});
    g_nwb = 0; flush(); rec_begin();
}
static void op_draw(uint32_t op, const void *a, uint32_t size) {
    p_draw dr; p_draw_idx di; int indexed = op == OP_DRAW_IDX;
    int prim; uint32_t count, instances, base_inst; int32_t base_vertex = 0; uint32_t first = 0; uint64_t ind, mdi_cnt; int mdi_i;
    if (indexed) { const p_draw_idx *d = draw_idx_rec(a, size, &di); prim = d->prim; count = (uint32_t)d->count; instances = (uint32_t)d->instances; base_inst = (uint32_t)d->base_instance; base_vertex = d->base_vertex; ind = d->ind; mdi_cnt = d->mdi_cnt; mdi_i = d->mdi_i; }
    else { const p_draw *d = draw_rec(a, size, &dr); prim = d->prim; count = (uint32_t)d->count; instances = (uint32_t)d->instances; base_inst = (uint32_t)d->base_instance; first = (uint32_t)d->first; ind = d->ind; mdi_cnt = d->mdi_cnt; mdi_i = d->mdi_i; }
    if (mdi_cnt) { uint32_t live = 0; void *bf; uint64_t of; if (mtl_addr(mdi_cnt, 4, &bf, &of)) memcpy(&live, (const void *)(uintptr_t)mdi_cnt, 4); if ((uint32_t)mdi_i >= live) return; }
    if (ind) {   // indirect: the arguments as they are in memory now (written by the GPU earlier in this submission they would be stale: logged once)
        uint32_t args[5] = {0}; void *bf; uint64_t of;
        if (!mtl_addr(ind, indexed ? 20 : 16, &bf, &of)) return;
        memcpy(args, (const void *)(uintptr_t)ind, indexed ? 20 : 16);
        if (indexed) { count = args[0]; instances = args[1]; first = args[2]; base_vertex = (int32_t)args[3]; base_inst = args[4]; }
        else { count = args[0]; instances = args[1]; first = args[2]; base_inst = args[3]; }
        static int n; if (!n++) VLOG("indirect draws: arguments read on the CPU when recorded");
    }
    if (!instances) instances = 1;
    if (!count || D.rast_discard) return;
    vshader *vs = shader_of(D.bst[0], 0), *ps = shader_of(D.bst[1], 1);
    if (!vs) return;
    if (vs->has_img || (ps && ps->has_img)) { static int n; if (!n++) VLOG("[RED] a draw with storage images: not executed yet on Vulkan"); return; }
    int topo = vk_topo(prim);
    if (topo == -1) { if (g_red_prim++ < 4) VLOG("[RED] draw primitive %d is not drawn on Vulkan", prim); return; }
    // the index list (u32 when widened or expanded)
    uint32_t *idx = NULL, nidx = count; int itype = indexed ? di.itype : 2;
    const p_draw_idx *dx = indexed ? draw_idx_rec(a, size, &di) : NULL;
    if (indexed) {
        unsigned isz = dx->itype == 0 ? 1 : dx->itype == 1 ? 2 : 4; uint64_t ib = dx->ibuf + (uint64_t)first * isz;
        void *bf; uint64_t of; if (!mtl_addr(ib, (uint64_t)count * isz, &bf, &of)) return;
        idx = malloc((size_t)count * 4);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)ib + (size_t)i * isz;
            uint32_t v = isz == 1 ? *p : isz == 2 ? *(const uint16_t *)p : *(const uint32_t *)p;
            idx[i] = D.restart.on && v == D.restart.index ? 0xffffffffu : v;   // the game's restart index → Vulkan's (all ones)
        }
        if (D.restart.on && topo >= 0 && topo != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {   // lists: a restart only drops what is unfinished; Vulkan 1.1 has no restart for lists
            uint32_t per = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 1 : topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? 2 : 3, m = 0, run = 0;
            for (uint32_t i = 0; i < count; i++) { if (idx[i] == 0xffffffffu) { m -= run % per; run = 0; continue; } idx[m++] = idx[i]; run++; }
            m -= run % per; nidx = m;
        }
        itype = 2;
    } else if (topo == -2) { idx = malloc((size_t)count * 4); for (uint32_t i = 0; i < count; i++) idx[i] = first + i; }
    if (topo == -2) {
        uint32_t m; uint32_t *e = prim_indices(prim, idx, nidx, D.restart.on, 0xffffffffu, &m, &topo);
        free(idx); idx = e; nidx = m; if (!nidx) { free(idx); return; }
    }
    // the vertex range the streams must hold
    uint32_t vmax = 0;
    if (idx) { for (uint32_t i = 0; i < nidx; i++) if (idx[i] != 0xffffffffu && idx[i] > vmax) vmax = idx[i]; }
    else vmax = first + count - 1;
    int64_t vlast = (int64_t)vmax + base_vertex; if (vlast < 0) vlast = 0;
    if (!pass_begin()) { free(idx); return; }
    // pipeline key
    pkey k; memset(&k, 0, sizeof k);
    k.vs = (uint64_t)(uintptr_t)vs; k.ps = (uint64_t)(uintptr_t)ps; k.rp = (uint64_t)g_prp; k.topo = (uint8_t)topo;
    k.restart = D.restart.on && (topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) && indexed;
    k.gl = !atomic_load(&g_depth_zero) && vs->mod_gl;
    k.cull = D.poly.cull; if (k.cull == 3) { free(idx); return; }   // FRONT_AND_BACK draws nothing
    int ll = atomic_load(&g_origin_ll);
    k.front = (uint8_t)(((D.poly.front == NVN_FRONT_FACE_CCW) != ll) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);   // as Metal's winding, in framebuffer space
    k.pmode = (uint8_t)(D.poly.mode == NVN_POLYGON_MODE_LINE && g_feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : D.poly.mode == NVN_POLYGON_MODE_POINT && g_feat.fillModeNonSolid ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
    k.dbias = (D.poly.offset_en & 4) != 0; k.dclamp = D.depth_clamp && g_feat.depthClamp; k.samples = (uint8_t)g_prk.samples; k.a2c = D.ms.a2c;
    if (g_prk.depth) {
        k.dt = D.ds.dt; k.dw = D.ds.dt && D.ds.dw; k.df = (uint8_t)(D.ds.df >= 1 && D.ds.df <= 8 ? D.ds.df - 1 : VK_COMPARE_OP_ALWAYS);
        int stencil = nvn_vk_aspect(g_prk.f[8]) & VK_IMAGE_ASPECT_STENCIL_BIT;
        k.st = D.ds.st && stencil;
        k.sf[0] = (uint8_t)(D.ds.sf_f - 1); k.sf[1] = (uint8_t)(D.ds.sf_b - 1);
        uint8_t ops[2][3] = {{(uint8_t)D.ds.fail_f, (uint8_t)D.ds.zfail_f, (uint8_t)D.ds.zpass_f}, {(uint8_t)D.ds.fail_b, (uint8_t)D.ds.zfail_b, (uint8_t)D.ds.zpass_b}};
        for (int f = 0; f < 2; f++) for (int j = 0; j < 3; j++) k.sop[f][j] = ops[f][j] >= 1 ? ops[f][j] - 1 : 0;
    } else k.df = VK_COMPARE_OP_ALWAYS;
    k.ncol = (uint8_t)g_prk.n;
    for (int i = 0; i < k.ncol; i++) {
        if (!g_prk.f[i]) continue;
        uint32_t m = D.chmask.mask >> (4 * i) & 0xf; if (ps && !(ps->out_mask >> i & 1)) m = 0; if (!ps) m = 0;
        const blend_st *b = &D.blend[i];
        int en = (D.color.blend_en >> i & 1) && m && !is_int_fmt(g_prk.f[i]) && !b->adv;
        if (en && (dual_src(b->src_rgb) || dual_src(b->dst_rgb) || dual_src(b->src_a) || dual_src(b->dst_a)) && (!g_feat.dualSrcBlend || i)) en = 0;
        k.col[i] = (typeof(k.col[i])){(uint8_t)en, (uint8_t)m, (uint8_t)bfac(b->src_rgb), (uint8_t)bfac(b->dst_rgb), (uint8_t)bfac(b->src_a), (uint8_t)bfac(b->dst_a), (uint8_t)bop(b->eq_rgb), (uint8_t)bop(b->eq_a)};
        if (!en) { k.col[i].sc = k.col[i].sa = VK_BLEND_FACTOR_ONE; k.col[i].dc = k.col[i].da = VK_BLEND_FACTOR_ZERO; k.col[i].oc = k.col[i].oa = VK_BLEND_OP_ADD; }
    }
    // vertex input: each attribute the shader reads, from its stream
    uint32_t used_streams = 0; uint32_t need[32] = {0}; int zero_b = -1;
    for (int loc = 0; loc < 32; loc++) {
        if (!(vs->in_mask >> loc & 1)) continue;
        va_st at; memcpy(&at, &D.attr[loc], 4); int want = vs->in_kind[loc];
        VkFormat f = loc < D.nattr && at.fmt ? vfmt((int)at.fmt, want) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp); if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) { static int n; if (n++ < 4) VLOG("[RED] vertex format %d (NVN %#x) not supported here: the attribute reads zeros", (int)f, at.fmt); f = VK_FORMAT_UNDEFINED; } }
        if (f == VK_FORMAT_UNDEFINED || !D.vbuf[at.stream].address) {   // unset: zeros, from a binding of its own
            zero_b = 32;
            k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, 32, (uint32_t)(want == 'u' ? VK_FORMAT_R32G32B32A32_UINT : want == 'i' ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT), 0};
            continue;
        }
        k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)loc, at.stream, (uint32_t)f, at.offset};
        used_streams |= 1u << at.stream; uint32_t end = at.offset + vfmt_size(f); if (end > need[at.stream]) need[at.stream] = end;
    }
    stage_t vbufs[33]; VkBuffer vb[33]; VkDeviceSize vo[33]; uint32_t nb = 0;
    for (int s = 0; s < 32; s++) {
        if (!(used_streams >> s & 1)) continue;
        uint32_t stride = D.stream[s].stride, div = D.stream[s].divisor;
        uint64_t last = div ? (base_inst + instances - 1) / div : (uint64_t)vlast;
        uint64_t bytes = stride ? last * stride + need[s] : need[s];
        if (bytes > D.vbuf[s].size && D.vbuf[s].size) bytes = D.vbuf[s].size;
        vbufs[s] = snap(D.vbuf[s].address, bytes, 16);
        if (div > 1) { static int n; if (!n++) VLOG("[RED] instance divisor %u: drawn as 1 (no VK_EXT_vertex_attribute_divisor yet)", div); }
        k.bind[k.nbind++] = (typeof(k.bind[0])){(uint32_t)s, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, stride, 0};
        vb[nb] = vbufs[s].buf; vo[nb] = vbufs[s].off; nb++;
    }
    VkBuffer zb = VK_NULL_HANDLE; VkDeviceSize zo = 0;
    if (zero_b >= 0) { stage_t z = zeros(); zb = z.buf; zo = z.off; k.bind[k.nbind++] = (typeof(k.bind[0])){32, VK_VERTEX_INPUT_RATE_INSTANCE, 0, 0}; }
    VkPipelineLayout lay = pipe_layout(vs->dsl, ps ? ps->dsl : VK_NULL_HANDLE);
    VkPipeline pl = pipeline(&k, lay, vs, ps);
    if (!pl) { free(idx); return; }
    // descriptors (texture uploads end the pass: pass_begin again)
    VkDescriptorSet sets[2]; sets[0] = stage_set(0, vs, base_inst, NULL); sets[1] = ps ? stage_set(1, ps, base_inst, NULL) : VK_NULL_HANDLE;
    if (!g_pass) { pass_begin(); if ((uint64_t)g_prp != k.rp) { free(idx); return; } }
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, ps ? 2 : 1, sets, 0, NULL);
    // viewport 0 (lower-left origin = positive, upper-left = negative height; a NEGATIVE_Y swizzle flips again), depth range, scissor, dynamic state
    float vx = 0, vy = 0, vw = (float)g_pext.width, vh = (float)g_pext.height;
    if (D.has_vp) { vx = D.vp[0][0]; vy = D.vp[0][1]; vw = D.vp[0][2]; vh = D.vp[0][3]; }
    int flip = !ll; if (D.vp_sw[0][1] == 1) flip = !flip;
    VkViewport v = {vx, flip ? vy + vh : vy, vw, flip ? -vh : vh, D.dr[0] < 0 ? 0 : D.dr[0] > 1 ? 1 : D.dr[0], D.dr[1] < 0 ? 0 : D.dr[1] > 1 ? 1 : D.dr[1]};
    vkCmdSetViewport(g_cb, 0, 1, &v);
    VkRect2D sc = {{0, 0}, g_pext};
    if (S.has_sc) {
        int x0 = S.sc.x < 0 ? 0 : S.sc.x, y0 = S.sc.y < 0 ? 0 : S.sc.y, x1 = S.sc.x + S.sc.w, y1 = S.sc.y + S.sc.h;
        if (x1 > (int)g_pext.width) x1 = (int)g_pext.width; if (y1 > (int)g_pext.height) y1 = (int)g_pext.height;
        if (x1 <= x0 || y1 <= y0) { free(idx); return; }   // an empty scissor draws nothing
        sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    vkCmdSetScissor(g_cb, 0, 1, &sc);
    vkCmdSetBlendConstants(g_cb, D.blend_color);
    vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, D.st_vmask[0]); vkCmdSetStencilCompareMask(g_cb, VK_STENCIL_FACE_BACK_BIT, D.st_vmask[1]);
    vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_FRONT_BIT, D.st_wmask[0]); vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_BACK_BIT, D.st_wmask[1]);
    vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_FRONT_AND_BACK, D.st_ref[0]);   // the front value for both faces, as Metal
    vkCmdSetDepthBias(g_cb, k.dbias ? D.po.units : 0, k.dbias ? D.po.clamp : 0, k.dbias ? D.po.factor : 0);
    vkCmdSetLineWidth(g_cb, 1);
    for (uint32_t i = 0, s = 0; s < 32; s++) if (used_streams >> s & 1) { vkCmdBindVertexBuffers(g_cb, s, 1, &vb[i], &vo[i]); i++; }
    if (zb) vkCmdBindVertexBuffers(g_cb, 32, 1, &zb, &zo);
    if (idx) {
        stage_t is = stage_al((VkDeviceSize)nidx * 4, 16); memcpy(is.p, idx, (size_t)nidx * 4); free(idx);
        vkCmdBindIndexBuffer(g_cb, is.buf, is.off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g_cb, nidx, instances, 0, indexed ? base_vertex : 0, base_inst);
    } else vkCmdDraw(g_cb, count, instances, first, base_inst);
    (void)itype;
    wb_finish();
}
static nvn_hmap g_cpipe = {.klen = 8};
static void op_dispatch(const p_dispatch *d) {
    vshader *cs = shader_of(D.cprog, 5);
    if (!cs) return;
    if (cs->has_img) { static int n; if (!n++) VLOG("[RED] a dispatch with storage images: not executed yet on Vulkan"); return; }
    uint32_t dims[3] = {(uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z};
    if (d->ind) { void *bf; uint64_t of; if (!mtl_addr(d->ind, 12, &bf, &of)) return; memcpy(dims, (const void *)(uintptr_t)d->ind, 12); }
    if (!dims[0] || !dims[1] || !dims[2]) return;
    pass_end();
    VkPipelineLayout lay = pipe_layout(cs->dsl, VK_NULL_HANDLE);
    uint64_t key = (uint64_t)(uintptr_t)cs; VkPipeline p = (VkPipeline)nvn_hm_get(&g_cpipe, &key);
    if (!p) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->mod, "main", NULL}, lay, VK_NULL_HANDLE, -1};
        if (vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) < 0) { VLOG("[RED] compute pipeline for %s failed", cs->key); p = VK_NULL_HANDLE; }
        nvn_hm_put(&g_cpipe, &key, p ? (void *)p : (void *)1);
    }
    if (!p || p == (VkPipeline)1) return;
    VkDescriptorSet set = stage_set(5, cs, 0, dims);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &set, 0, NULL);
    vkCmdDispatch(g_cb, dims[0], dims[1], dims[2]);
    barrier();
    wb_finish();
}
