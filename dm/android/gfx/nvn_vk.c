// Vulkan execution layer of the NVN layer on Android: the interface dm/gfx/nvn/hle_nvn.c calls in nvn_mtl.m (nvn_mtl.h, nvn_obj.h), on Vulkan 1.1.
// Stage 1 of the backend (dm/android/README.md, Roadmap): textures, copies, clears and presentation; draws and dispatches are not executed yet (logged once).
//   · Device: the first physical device, one graphics queue; BC and ASTC sampling when the device has them. DM_VK_VALIDATE=1 turns on the Khronos validation layer (log lines [vk]).
//   · Memory: device-local and host-visible blocks of 64 MB, first fit with merging; staging space per submission in host-visible chunks, reused once the submission completed.
//   · Textures: one VkImage per texture serial (the snapshot each command record carries), made at first use, always in layout GENERAL (stage 1: simple, not the fastest on tilers).
//     A packaged texture (a layout given by the game) or one the CPU writes (mtl_tex_cpu_written) is uploaded from its storage in guest memory, block-linear deswizzled on the CPU
//     (dm/gfx/tex/nvn_deswizzle.c); a CPU-written one again in each submission that uses it. 3-component formats are widened, and BC formats are decoded on the CPU (bcdec) when the device
//     cannot sample them. A texture rendered into, cleared or copied into is the GPU's from then on (no more uploads).
//   · Submissions run in order and complete before mtl_sub_commit returns (stage 1: correct and simple; the CPU waits for the GPU). The CPU-visible results (event writes, counter reports,
//     texture-to-buffer copies) are applied in record order once the GPU finished; buffer writes (inline updates, buffer copies and fills) are made by the CPU in record order, after
//     the GPU work before them when a readback is pending. Buffer-to-texture copies take their source when recorded.
//   · Present: the texture blitted into the swapchain image of the window (front_metal_layer: the ANativeWindow), letterboxed to its aspect, flipped in the lower-left window origin,
//     the crop rectangle applied; MAILBOX when the device has it, else FIFO. Without a window (tests) the frame goes to a host-visible image instead (nvn_vk_test_frame).
#include "../../gfx/nvn/nvn_obj.h"
#include "../../gfx/nvn/nvn_mtl.h"
#include "../../gfx/nvn/nvn_hmap.h"
#include "../../gfx/nvn/nvn_timestamp.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include "nvn_fmt_vk.h"
#include <vulkan/vulkan_core.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../third_party/bcdec/bcdec.h"
#pragma clang diagnostic pop
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VLOG(...) do { fprintf(stderr, "[dm] [nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VDIE(...) do { VLOG("FATAL " __VA_ARGS__); abort(); } while (0)
#define VKC(x) do { VkResult r_ = (x); if (r_ < 0) VDIE("%s failed: %d", #x, (int)r_); } while (0)
uint64_t guest_ns(void);              // dm/host/hle_os.c
void *front_metal_layer(void);        // the front end's window (front_android.c: the ANativeWindow), NULL without one

static pthread_mutex_t g_mu;           // every entry point; held from mtl_sub_begin to mtl_sub_commit (one submission is recorded by one thread)
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceMemoryProperties g_memp; static VkPhysicalDeviceProperties g_props;
static int g_bc, g_astc, g_validate, g_aniso, g_mirror_clamp;
static VkDebugUtilsMessengerEXT g_msgr;
static uint64_t g_fence;               // submissions committed (all complete)

// ---------- Memory: 64 MB blocks per memory type and kind (images / buffers, kept apart for bufferImageGranularity), first fit ----------
enum { BLOCK = 64 << 20, MAXBLK = 256 };
typedef struct { VkDeviceSize off, size; } span;
typedef struct { VkDeviceMemory mem; uint32_t type; int kind; VkDeviceSize size; void *map; span *free; int nfree, capfree; } vblock;
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; void *map; int blk; } vmem;   // blk -1: a dedicated allocation
static vblock g_blk[MAXBLK]; static int g_nblk;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
static void free_insert(vblock *b, VkDeviceSize off, VkDeviceSize size) {
    int i = 0; while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) { b->capfree = b->capfree ? 2 * b->capfree : 16; b->free = realloc(b->free, (size_t)b->capfree * sizeof *b->free); }
    memmove(b->free + i + 1, b->free + i, (size_t)(b->nfree - i) * sizeof *b->free); b->free[i] = (span){off, size}; b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) { b->free[i].size += b->free[i + 1].size; memmove(b->free + i + 1, b->free + i + 2, (size_t)(b->nfree - i - 2) * sizeof *b->free); b->nfree--; }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) { b->free[i - 1].size += b->free[i].size; memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--; }
}
static vmem mem_alloc(VkMemoryRequirements rq, VkMemoryPropertyFlags want, int kind) {
    uint32_t t = mem_type(rq.memoryTypeBits, want);
    if (t == ~0u && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = mem_type(rq.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == ~0u) VDIE("no memory type for bits %#x properties %#x", rq.memoryTypeBits, want);
    int host = (g_memp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (rq.size > BLOCK / 2) {   // large: its own allocation
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, t};
        vmem a = {0, 0, rq.size, NULL, -1}; VKC(vkAllocateMemory(g_dev, &ai, NULL, &a.mem));
        if (host) VKC(vkMapMemory(g_dev, a.mem, 0, VK_WHOLE_SIZE, 0, &a.map));
        return a;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int bi = 0; bi < g_nblk; bi++) {
            vblock *b = &g_blk[bi]; if (!b->mem || b->type != t || b->kind != kind) continue;
            for (int i = 0; i < b->nfree; i++) {
                VkDeviceSize o = (b->free[i].off + rq.alignment - 1) & ~(rq.alignment - 1), end = b->free[i].off + b->free[i].size;
                if (o + rq.size > end) continue;
                span s = b->free[i]; memmove(b->free + i, b->free + i + 1, (size_t)(b->nfree - i - 1) * sizeof *b->free); b->nfree--;
                if (o > s.off) free_insert(b, s.off, o - s.off);
                if (o + rq.size < end) free_insert(b, o + rq.size, end - o - rq.size);
                return (vmem){b->mem, o, rq.size, b->map ? (uint8_t *)b->map + o : NULL, bi};
            }
        }
        if (pass) break;
        int bi = 0; while (bi < g_nblk && g_blk[bi].mem) bi++;
        if (bi == MAXBLK) VDIE("more than %d memory blocks", MAXBLK);
        if (bi == g_nblk) g_nblk++;
        vblock *b = &g_blk[bi]; memset(b, 0, sizeof *b); b->type = t; b->kind = kind; b->size = BLOCK;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, BLOCK, t};
        VkResult r = vkAllocateMemory(g_dev, &ai, NULL, &b->mem);
        if (r < 0) VDIE("out of GPU memory (a %d MB block, memory type %u: %d)", BLOCK >> 20, t, (int)r);
        if (host) VKC(vkMapMemory(g_dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
        free_insert(b, 0, BLOCK);
    }
    VDIE("memory allocation of %llu bytes failed", (unsigned long long)rq.size);
}
static void mem_free(vmem *a) {
    if (!a->mem) return;
    if (a->blk < 0) vkFreeMemory(g_dev, a->mem, NULL);
    else free_insert(&g_blk[a->blk], a->off, a->size);
    memset(a, 0, sizeof *a);
}

// ---------- Staging: host-visible chunks for one submission, reused after it completed ----------
typedef struct { VkBuffer buf; vmem m; VkDeviceSize size, used; } chunk;
static chunk g_chunk[64]; static int g_nchunk;
typedef struct { VkBuffer buf; VkDeviceSize off; uint8_t *p; } stage_t;
static stage_t stage(VkDeviceSize n) {
    n = (n + 15) & ~(VkDeviceSize)15;
    for (int i = 0; i < g_nchunk; i++) if (g_chunk[i].size - g_chunk[i].used >= n) {
        chunk *c = &g_chunk[i]; stage_t s = {c->buf, c->used, (uint8_t *)c->m.map + c->used}; c->used += n; return s;
    }
    if (g_nchunk == 64) VDIE("staging: more than 64 chunks in one submission");
    chunk *c = &g_chunk[g_nchunk++]; c->size = n > (16 << 20) ? n : 16 << 20; c->used = 0;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, c->size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VKC(vkCreateBuffer(g_dev, &bi, NULL, &c->buf));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(g_dev, c->buf, &rq);
    c->m = mem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1);
    VKC(vkBindBufferMemory(g_dev, c->buf, c->m.mem, c->m.off));
    stage_t st = {c->buf, 0, c->m.map}; c->used = n; return st;
}
static void stage_reset(void) {
    for (int i = 0; i < g_nchunk; i++) {
        if (g_chunk[i].size > (16 << 20)) { vkDestroyBuffer(g_dev, g_chunk[i].buf, NULL); mem_free(&g_chunk[i].m); g_chunk[i--] = g_chunk[--g_nchunk]; continue; }   // big ones are not kept
        g_chunk[i].used = 0;
    }
}

// ---------- Transient objects (views, framebuffers) destroyed when the submission completed ----------
typedef struct { int kind; uint64_t h; } trans;   // 0 image view, 1 framebuffer
static trans *g_trans; static int g_ntrans, g_captrans;
static void trans_add(int kind, uint64_t h) { if (g_ntrans == g_captrans) { g_captrans = g_captrans ? 2 * g_captrans : 64; g_trans = realloc(g_trans, (size_t)g_captrans * sizeof *g_trans); } g_trans[g_ntrans++] = (trans){kind, h}; }
static void trans_free(void) {
    for (int i = 0; i < g_ntrans; i++) {
        if (g_trans[i].kind == 0) vkDestroyImageView(g_dev, (VkImageView)g_trans[i].h, NULL);
        else vkDestroyFramebuffer(g_dev, (VkFramebuffer)g_trans[i].h, NULL);
    }
    g_ntrans = 0;
}

// ---------- Device ----------
static VKAPI_ATTR VkBool32 VKAPI_CALL on_vk_msg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u; static atomic_int n;
    if (atomic_fetch_add(&n, 1) < 200) VLOG("[vk]%s %s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? " [RED]" : "", d->pMessage);
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { extern atomic_int nvn_vk_validation_errors; atomic_fetch_add(&nvn_vk_validation_errors, 1); }
    return VK_FALSE;
}
atomic_int nvn_vk_validation_errors;
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static VkCommandPool g_cpool; static VkCommandBuffer g_cb; static VkFence g_done; static int g_rec;
static void dev_init(void) {
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_mu, &ma);
    const char *ve = getenv("DM_VK_VALIDATE"); g_validate = ve && atoi(ve);
    uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
    VkExtensionProperties *ie = calloc(ne ? ne : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &ne, ie);
    const char *iext[4]; uint32_t nie = 0;
    if (has_ext(ie, ne, VK_KHR_SURFACE_EXTENSION_NAME)) iext[nie++] = VK_KHR_SURFACE_EXTENSION_NAME;
#ifdef __ANDROID__
    iext[nie++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif
    int dbg = g_validate && has_ext(ie, ne, VK_EXT_DEBUG_UTILS_EXTENSION_NAME); if (dbg) iext[nie++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    free(ie);
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative", 1, "nxnative", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, g_validate ? 1 : 0, &layer, nie, iext};
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r == VK_ERROR_LAYER_NOT_PRESENT) { VLOG("[RED] DM_VK_VALIDATE: no validation layer on this device, running without it"); ici.enabledLayerCount = 0; g_validate = 0; r = vkCreateInstance(&ici, NULL, &g_inst); }
    if (r < 0) VDIE("vkCreateInstance: %d (no Vulkan 1.1?)", (int)r);
    if (dbg && g_validate) {
        PFN_vkCreateDebugUtilsMessengerEXT cm = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, on_vk_msg, NULL};
        if (cm) cm(g_inst, &mi, NULL, &g_msgr);
    }
    uint32_t npd = 0; vkEnumeratePhysicalDevices(g_inst, &npd, NULL);
    if (!npd) VDIE("no Vulkan device");
    VkPhysicalDevice pds[8]; if (npd > 8) npd = 8; vkEnumeratePhysicalDevices(g_inst, &npd, pds); g_pd = pds[0];
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memp);
    VkPhysicalDeviceFeatures have; vkGetPhysicalDeviceFeatures(g_pd, &have);
    uint32_t nqf = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nqf, NULL);
    VkQueueFamilyProperties qfp[16]; if (nqf > 16) nqf = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nqf, qfp);
    g_qf = ~0u; for (uint32_t i = 0; i < nqf; i++) if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qf = i; break; }
    if (g_qf == ~0u) VDIE("no graphics queue");
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &nde, de);
    const char *dext[4]; uint32_t ndx = 0;
    if (has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) dext[ndx++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    free(de);
    VkPhysicalDeviceFeatures want = {0};
    want.textureCompressionBC = have.textureCompressionBC; want.textureCompressionASTC_LDR = have.textureCompressionASTC_LDR;
    want.samplerAnisotropy = have.samplerAnisotropy; want.independentBlend = have.independentBlend; want.imageCubeArray = have.imageCubeArray;
    want.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics; want.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
    g_bc = have.textureCompressionBC && !getenv("DM_VK_NO_BC"); g_astc = have.textureCompressionASTC_LDR; g_aniso = have.samplerAnisotropy;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, ndx, dext, &want};
    VKC(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
    VKC(vkCreateCommandPool(g_dev, &pci, NULL, &g_cpool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKC(vkAllocateCommandBuffers(g_dev, &cai, &g_cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0}; VKC(vkCreateFence(g_dev, &fci, NULL, &g_done));
    VLOG("Vulkan %u.%u.%u on %s (driver %#x): BC %s, ASTC %s%s", VK_VERSION_MAJOR(g_props.apiVersion), VK_VERSION_MINOR(g_props.apiVersion), VK_VERSION_PATCH(g_props.apiVersion),
         g_props.deviceName, g_props.driverVersion, g_bc ? "sampled" : "decoded on the CPU", g_astc ? "yes" : "no", g_validate ? "; validation on" : "");
}
static void dev(void) { pthread_once(&g_once, dev_init); }
static void lock(void) { dev(); pthread_mutex_lock(&g_mu); }
static void unlock(void) { pthread_mutex_unlock(&g_mu); }

// ---------- Recording ----------
static void rec_begin(void) {
    if (g_rec) return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKC(vkBeginCommandBuffer(g_cb, &bi)); g_rec = 1;
}
static void barrier(void) {   // stage 1: every transfer / clear is ordered against everything before and after it
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// CPU-visible results applied once the GPU finished, in record order
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; uint8_t *src; uint32_t rows, row_bytes, stride, slices; uint64_t slice_stride; } post_t;   // kind 1 event, 2 counter, 3 readback
static post_t *g_post; static int g_npost, g_cappost, g_readbacks;
static void post_add(post_t p) { if (g_npost == g_cappost) { g_cappost = g_cappost ? 2 * g_cappost : 64; g_post = realloc(g_post, (size_t)g_cappost * sizeof *g_post); } g_post[g_npost++] = p; if (p.kind == 3) g_readbacks++; }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return nvn_timestamp_from_ns(g); }
static void posts_run(void) {
    for (int i = 0; i < g_npost; i++) {
        post_t *p = &g_post[i];
        if (p->kind == 1) { if (p->mode == 1) *p->val += p->v; else *p->val = p->v; }
        else if (p->kind == 2) { uint64_t t = gpu_ticks(), rec[2] = {p->type == 0 || p->type == 0x10 ? t : 0, t}; if (p->addr) memcpy((void *)(uintptr_t)p->addr, rec, sizeof rec); }
        else for (uint32_t z = 0; z < p->slices; z++) for (uint32_t y = 0; y < p->rows; y++)
            memcpy((uint8_t *)(uintptr_t)p->addr + z * p->slice_stride + (uint64_t)y * p->stride, p->src + ((uint64_t)z * p->rows + y) * p->row_bytes, p->row_bytes);
    }
    g_npost = 0; g_readbacks = 0;
}
static void flush(void) {   // submit what is recorded, wait, then the CPU-visible results and the transient objects
    if (!g_rec) { posts_run(); return; }
    VKC(vkEndCommandBuffer(g_cb)); g_rec = 0;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_cb, 0, NULL};
    VKC(vkQueueSubmit(g_q, 1, &si, g_done));
    VkResult r = vkWaitForFences(g_dev, 1, &g_done, VK_TRUE, 10000000000ull);
    if (r == VK_TIMEOUT) VDIE("the GPU did not finish a submission in 10 s");
    VKC(r); VKC(vkResetFences(g_dev, 1, &g_done));
    VKC(vkResetCommandBuffer(g_cb, 0));
    posts_run(); trans_free(); stage_reset();
}

// ---------- Textures ----------
typedef struct vtex {
    uint64_t serial; tex_t t;
    VkImage img; vmem m; VkFormat fmt; nvn_vkfmt f; int decoded;   // decoded: BC decoded to fmt on the CPU (f.bc)
    VkImageAspectFlags aspect; VkImageType type; uint32_t w, h, d, layers, levels; VkSampleCountFlagBits samples; VkImageCreateFlags cflags; VkImageUsageFlags usage;
    int gpu_owned, uploaded, cpu_written; uint64_t upload_sub;
} vtex;
static nvn_hmap g_tex = {.klen = 8};
static uint64_t g_sub;   // recorded submissions (counted at mtl_sub_begin): a CPU-written texture is uploaded once in each
static int g_red_draw, g_red_fmt, g_red_mask;
static VkFormat bc_decoded(int bc, VkFormat f) {
    switch (bc) {
        case 4: return f == VK_FORMAT_BC4_SNORM_BLOCK ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_UNORM;
        case 5: return f == VK_FORMAT_BC5_SNORM_BLOCK ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_R8G8_UNORM;
        case 6: return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: return f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC2_SRGB_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
}
static int fmt_ok(VkFormat f, VkImageType type, VkImageUsageFlags usage, VkImageCreateFlags cf, VkSampleCountFlagBits s) {
    VkImageFormatProperties p;
    if (vkGetPhysicalDeviceImageFormatProperties(g_pd, f, type, VK_IMAGE_TILING_OPTIMAL, usage, cf, &p) != VK_SUCCESS) return 0;
    return (p.sampleCounts & s) != 0;
}
static nvn_hmap g_views;
static void tex_free(vtex *v) {
    for (uint32_t i = 0; i < g_views.cap;) {   // its sampled views (keys begin with the serial)
        uint64_t sr; memcpy(&sr, g_views.keys + (size_t)i * g_views.klen, 8);
        if (g_views.vals[i] && sr == v->serial) { VkImageView iv = (VkImageView)nvn_hm_del(&g_views, g_views.keys + (size_t)i * g_views.klen); vkDestroyImageView(g_dev, iv, NULL); }
        else i++;
    }
    if (v->img) vkDestroyImage(g_dev, v->img, NULL); mem_free(&v->m); free(v);
}
static vtex *tex_make(const tex_t *t) {
    const nvn_texdesc *d = &t->d;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    nvn_vkfmt f = nvn_fmt_to_vk(d->format);
    if (!f.kind) { if (g_red_fmt++ < 8) VLOG("[RED] texture format %#x has no Vulkan counterpart: texture ser=%llu not made", d->format, (unsigned long long)t->serial); return NULL; }
    vtex *v = calloc(1, sizeof *v); v->serial = t->serial; v->t = *t; v->f = f;
    v->w = d->width ? d->width : 1; v->h = d->height ? d->height : 1; v->d = 1; v->layers = 1; v->levels = d->levels ? d->levels : 1; v->type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
        case NVN_TEXTURE_TARGET_1D: v->h = 1; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: v->layers = v->h; v->h = 1; break;
        case NVN_TEXTURE_TARGET_3D: v->type = VK_IMAGE_TYPE_3D; v->d = d->depth ? d->depth : 1; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: v->layers = d->depth ? d->depth : 1; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: v->layers = 6; v->cflags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: v->layers = d->depth > 6 ? d->depth : 6; v->cflags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        default: break;
    }
    v->samples = d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT;
    v->fmt = f.vk;
    if (f.bc && !g_bc) { v->fmt = bc_decoded(f.bc, f.vk); v->decoded = 1; }
    if (f.vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f.vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !g_astc) VLOG("[RED] ASTC texture ser=%llu on a device without ASTC: not sampled right", (unsigned long long)t->serial);
    v->aspect = nvn_vk_aspect(v->fmt); if (!v->aspect) v->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags base = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags att = v->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    int compressed = (v->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && v->fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    v->usage = base | (compressed ? 0 : att);
    if (!fmt_ok(v->fmt, v->type, v->usage, v->cflags, v->samples)) v->usage = base;
    if (!fmt_ok(v->fmt, v->type, v->usage, v->cflags, v->samples) && f.alt) { v->fmt = f.alt; v->aspect = nvn_vk_aspect(v->fmt); v->usage = base | att; if (!fmt_ok(v->fmt, v->type, v->usage, v->cflags, v->samples)) v->usage = base; }
    if (!fmt_ok(v->fmt, v->type, v->usage, v->cflags, v->samples)) {
        if (g_red_fmt++ < 8) VLOG("[RED] texture ser=%llu: format %#x (Vulkan %d) %ux%ux%u not supported here", (unsigned long long)t->serial, d->format, (int)v->fmt, v->w, v->h, v->d);
        free(v); return NULL;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, v->cflags, v->type, v->fmt, {v->w, v->h, v->d}, v->levels, v->layers, v->samples,
                            VK_IMAGE_TILING_OPTIMAL, v->usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(vkCreateImage(g_dev, &ci, NULL, &v->img));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, v->img, &rq);
    v->m = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(g_dev, v->img, v->m.mem, v->m.off));
    // into GENERAL, contents zero (a texture the game never wrote reads as 0, as the GPU's fresh memory does)
    rec_begin();
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, v->img, {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkImageSubresourceRange all = {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (compressed) ;   // a compressed image cannot be cleared: its content is undefined until uploaded
    else if (v->aspect == VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(g_cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all); }
    else { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(g_cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all); }
    barrier();
    return v;
}

// Texel conversion for an upload: rows of source blocks (bpb bytes each) → what the image takes. Returns the bytes per destination row (of texels or of blocks).
static uint32_t one_bits(VkFormat f, int comp) {   // the value of 1 in a component of the widened formats (alpha of a 3-component texture)
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return 0xff; case VK_FORMAT_R8G8B8A8_SNORM: return 0x7f;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 0x3c00; case VK_FORMAT_R16G16B16A16_UNORM: return 0xffff; case VK_FORMAT_R16G16B16A16_SNORM: return 0x7fff;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 0x3f800000;
        default: (void)comp; return 1;   // integer formats: raw 1
    }
}
static void widen(const uint8_t *src, uint8_t *dst, uint32_t n, unsigned cb, VkFormat f) {   // n texels of 3 components of cb bytes → 4 components
    uint32_t one = one_bits(f, 3);
    for (uint32_t i = 0; i < n; i++) { memcpy(dst + i * 4 * cb, src + i * 3 * cb, 3 * cb); memcpy(dst + i * 4 * cb + 3 * cb, &one, cb); }
}
static void bc_block(int bc, VkFormat f, const uint8_t *blk, uint8_t *dst, int pitch) {
    switch (bc) {
        case 1: bcdec_bc1(blk, dst, pitch); break;
        case 2: bcdec_bc2(blk, dst, pitch); break;
        case 3: bcdec_bc3(blk, dst, pitch); break;
        case 4: bcdec_bc4(blk, dst, pitch, f == VK_FORMAT_BC4_SNORM_BLOCK); break;
        case 5: bcdec_bc5(blk, dst, pitch, f == VK_FORMAT_BC5_SNORM_BLOCK); break;
        case 6: { uint16_t rgb[16 * 3]; bcdec_bc6h_half(blk, rgb, 4 * 3, f == VK_FORMAT_BC6H_SFLOAT_BLOCK);   // 3 halves a texel → RGBA16F
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint16_t *o = (uint16_t *)(dst + y * pitch) + 4 * x; memcpy(o, rgb + 3 * (4 * y + x), 6); o[3] = 0x3c00; } break; }
        default: bcdec_bc7(blk, dst, pitch); break;
    }
}
static unsigned decoded_texel_bytes(int bc) { return bc == 4 ? 1 : bc == 5 ? 2 : bc == 6 ? 8 : 4; }
// Copy blocks_w × block_rows × slices source blocks (src_pitch bytes a row, src_slice bytes a slice) into staging as the image takes them; *row_len / *img_h: bufferRowLength / ImageHeight in texels
static stage_t convert(const vtex *v, const uint8_t *src, uint32_t blocks_w, uint32_t block_rows, uint32_t slices, size_t src_pitch, size_t src_slice, uint32_t *row_len, uint32_t *img_h) {
    int bw, bh, bpb = nvn_fmt_bpb(v->t.d.format, &bw, &bh);
    if (v->decoded) {
        unsigned tb = decoded_texel_bytes(v->f.bc); uint32_t pitch = blocks_w * 4 * tb;
        stage_t s = stage((VkDeviceSize)pitch * block_rows * 4 * slices);
        for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < block_rows; y++) for (uint32_t x = 0; x < blocks_w; x++)
            bc_block(v->f.bc, v->f.vk, src + z * src_slice + y * src_pitch + (size_t)x * bpb, s.p + ((size_t)z * block_rows * 4 + y * 4) * pitch + (size_t)x * 4 * tb, (int)pitch);
        *row_len = blocks_w * 4; *img_h = block_rows * 4; return s;
    }
    if (v->f.expand) {
        unsigned cb = v->f.expand; uint32_t pitch = blocks_w * 4 * cb;
        stage_t s = stage((VkDeviceSize)pitch * block_rows * slices);
        for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < block_rows; y++) widen(src + z * src_slice + y * src_pitch, s.p + ((size_t)z * block_rows + y) * pitch, blocks_w, cb, v->fmt);
        *row_len = blocks_w; *img_h = block_rows; return s;
    }
    uint32_t rb = blocks_w * (uint32_t)bpb;
    stage_t s = stage((VkDeviceSize)rb * block_rows * slices);
    for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < block_rows; y++) memcpy(s.p + ((size_t)z * block_rows + y) * rb, src + z * src_slice + y * src_pitch, rb);
    *row_len = blocks_w * (uint32_t)bw; *img_h = block_rows * (uint32_t)bh; return s;
}
static void lvl_dims(const vtex *v, unsigned l, uint32_t *w, uint32_t *h, uint32_t *d) { *w = v->w >> l ? v->w >> l : 1; *h = v->h >> l ? v->h >> l : 1; *d = v->d >> l ? v->d >> l : 1; }
static void upload(vtex *v) {   // the whole texture from its storage in guest memory
    const tex_t *t = &v->t; const nvn_texdesc *d = &t->d;
    if (!t->mem || v->samples > 1) return;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh); if (!bpb) return;
    rec_begin();
    size_t tmpcap = 0; uint8_t *tmp = NULL;
    int linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    unsigned levels = linear ? 1 : v->levels, layers = v->type == VK_IMAGE_TYPE_3D ? 1 : v->layers;
    for (unsigned l = 0; l < levels; l++) for (unsigned a = 0; a < layers; a++) {
        uint32_t w, h, dd; lvl_dims(v, l, &w, &h, &dd);
        uint32_t bxw = (w + (uint32_t)bw - 1) / (uint32_t)bw, brows = (h + (uint32_t)bh - 1) / (uint32_t)bh;
        size_t pitch = (size_t)bxw * bpb, slice = pitch * brows;
        const uint8_t *src;
        if (linear) {
            nvn_texlayout L; nvn_tex_layout(d, &L);
            size_t stride = d->stride ? d->stride : (pitch + L.align - 1) & ~(size_t)(L.align - 1);
            src = t->mem; pitch = stride; slice = stride * h;   // the driver counts pixel rows (nvn_tex.c)
        } else {
            if (tmpcap < slice * dd) { free(tmp); tmpcap = slice * dd; tmp = malloc(tmpcap); }
            if (nvn_deswizzle(d, l, a, t->mem, tmp, pitch)) continue;
            src = tmp;
        }
        uint32_t row_len, img_h; stage_t s = convert(v, src, bxw, brows, dd, pitch, slice, &row_len, &img_h);
        VkBufferImageCopy c = {s.off, row_len, img_h, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, l, a, 1}, {0, 0, 0}, {w, h, dd}};
        vkCmdCopyBufferToImage(g_cb, s.buf, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    free(tmp);
    barrier();
    v->uploaded = 1; v->upload_sub = g_sub;
}
static int tex_has_content(const vtex *v) { return v->t.has_layout || v->cpu_written; }
// The texture of a command record's snapshot. dst: it is about to be written by the GPU (it becomes the GPU's)
static vtex *tex_use(const tex_t *t, int dst) {
    if (!t || !t->serial) return NULL;
    vtex *v = nvn_hm_get(&g_tex, &t->serial);
    if (v && (v->t.d.width != t->d.width || v->t.d.height != t->d.height || v->t.d.format != t->d.format || v->t.d.target != t->d.target || v->t.d.levels != t->d.levels)) {   // re-initialized under the same serial
        nvn_hm_del(&g_tex, &t->serial); flush(); tex_free(v); v = NULL;
    }
    if (!v) { v = tex_make(t); if (!v) return NULL; nvn_hm_put(&g_tex, &t->serial, v); }
    v->t.mem = t->mem;   // the storage may move with the snapshot
    if (!v->gpu_owned && tex_has_content(v) && (!v->uploaded || (v->cpu_written && v->upload_sub != g_sub))) upload(v);
    if (dst) v->gpu_owned = 1;
    return v;
}

// ---------- Views and one-attachment render passes (clears) ----------
static VkImageView view_make(const vtex *v, unsigned level, unsigned layer, unsigned nlayers, VkImageAspectFlags aspect) {
    VkImageViewType vt = v->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->img, vt, v->fmt, {0}, {aspect, level, 1, layer, nlayers}};
    VkImageView iv; VKC(vkCreateImageView(g_dev, &ci, NULL, &iv)); trans_add(0, (uint64_t)iv); return iv;
}
static nvn_hmap g_rp = {.klen = 8};   // (format, samples) → render pass loading and storing its one attachment in GENERAL
static VkRenderPass rp_get(VkFormat f, VkSampleCountFlagBits s, int depth) {
    uint64_t k = (uint64_t)f << 8 | (uint64_t)s;
    VkRenderPass rp = (VkRenderPass)nvn_hm_get(&g_rp, &k); if (rp) return rp;
    VkAttachmentDescription ad = {0, f, s, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, depth ? 0 : 1, depth ? NULL : &ar, NULL, depth ? &ar : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 0, NULL};
    VKC(vkCreateRenderPass(g_dev, &ci, NULL, &rp)); nvn_hm_put(&g_rp, &k, (void *)rp); return rp;
}
// Clear a rectangle of one level and layers of a texture through a render pass (the scissor and partial regions vkCmdClear*Image cannot do)
static void clear_rect(const vtex *v, unsigned level, unsigned layer, unsigned nlayers, VkRect2D rc, const VkClearValue *cv, VkImageAspectFlags aspect) {
    uint32_t w, h, d; lvl_dims(v, level, &w, &h, &d);
    if (v->type == VK_IMAGE_TYPE_3D) { layer = 0; nlayers = 1; }   // stage 1: a 3D target's slice 0
    int x0 = rc.offset.x < 0 ? 0 : rc.offset.x, y0 = rc.offset.y < 0 ? 0 : rc.offset.y;
    int x1 = rc.offset.x + (int)rc.extent.width, y1 = rc.offset.y + (int)rc.extent.height; if (x1 > (int)w) x1 = (int)w; if (y1 > (int)h) y1 = (int)h;
    if (x1 <= x0 || y1 <= y0) return;
    int depth = v->aspect != VK_IMAGE_ASPECT_COLOR_BIT;
    VkRenderPass rp = rp_get(v->fmt, v->samples, depth);
    VkImageView iv = view_make(v, level, layer, nlayers, v->aspect);
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &iv, w, h, nlayers};
    VkFramebuffer fb; VKC(vkCreateFramebuffer(g_dev, &fi, NULL, &fb)); trans_add(1, (uint64_t)fb);
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {w, h}}, 0, NULL};
    vkCmdBeginRenderPass(g_cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca = {aspect, 0, *cv};
    VkClearRect cr = {{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, nlayers};
    vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(g_cb);
    barrier();
}

// ---------- Samplers (one per distinct parameter set) and sampled views (one per texture, view and swizzle) ----------
static nvn_hmap g_samp = {.klen = sizeof(samp_par)};

static VkSamplerAddressMode wrap_mode(int w) {
    switch (w) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 2: case 3: case 4: return g_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;   // MIRROR_CLAMP(_TO_BORDER): the edge variant
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // CLAMP (GL's, half border) and CLAMP_TO_EDGE
    }
}
static VkBorderColor border_color(const float b[4]) {   // Vulkan 1.1 has three fixed border colours: the nearest
    if (b[3] < 0.5f) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    return b[0] + b[1] + b[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}
static VkSampler sampler_get(const samp_par *p) {
    samp_par k; memset(&k, 0, sizeof k); k = *p;
    VkSampler sm = (VkSampler)nvn_hm_get(&g_samp, &k); if (sm) return sm;
    int mip = p->minf >= 2, lmin = p->minf == 1 || p->minf == 3 || p->minf == 5, lmip = p->minf == 4 || p->minf == 5;
    float lmn = p->lod_min, lmx = mip ? p->lod_max : lmn + 0.25f;   // no mipmapping: the base level only (Vulkan's recipe: maxLod = minLod + 0.25 with NEAREST)
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, lmin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              lmip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, wrap_mode(p->ws), wrap_mode(p->wt), wrap_mode(p->wr), p->lod_bias,
                              g_aniso && p->aniso > 1, p->aniso > 16 ? 16 : p->aniso < 1 ? 1 : p->aniso, p->cmp_mode == 1, p->cmp_func >= 1 && p->cmp_func <= 8 ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_ALWAYS,
                              lmn, lmx < lmn ? lmn : lmx, border_color(p->border), VK_FALSE};
    VKC(vkCreateSampler(g_dev, &ci, NULL, &sm)); nvn_hm_put(&g_samp, &k, (void *)sm); return sm;
}
// A view to sample: the texture's levels / layers / format as the NVN view narrows them, the format table's swizzle composed with the view's or the texture's own
typedef struct { uint64_t serial; view_t v; uint16_t swz; uint8_t has, stencil; } vkey;
static nvn_hmap g_views = {.klen = sizeof(vkey)};   // (declared before tex_free)
static VkComponentSwizzle nvn_swz(int c) { static const VkComponentSwizzle m[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A}; return c >= 0 && c < 6 ? m[c] : VK_COMPONENT_SWIZZLE_IDENTITY; }
static VkComponentSwizzle compose(VkComponentSwizzle outer, const VkComponentSwizzle inner[4], int self) {   // outer picks a component of what inner gives
    if (outer == VK_COMPONENT_SWIZZLE_IDENTITY) outer = (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + self);
    if (outer < VK_COMPONENT_SWIZZLE_R) return outer;
    int k = outer - VK_COMPONENT_SWIZZLE_R; VkComponentSwizzle r = inner[k];
    return r == VK_COMPONENT_SWIZZLE_IDENTITY ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + k) : r;
}
static VkImageViewType view_type(const vtex *v, int target, unsigned nlayers) {
    switch (target) {
        case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
        case NVN_TEXTURE_TARGET_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case NVN_TEXTURE_TARGET_1D_ARRAY: case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        default: return v->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
static VkImageView sampled_view(vtex *v, const view_t *vw, int has) {
    vkey k; memset(&k, 0, sizeof k); k.serial = v->serial; k.has = (uint8_t)has; if (has) k.v = *vw; k.swz = v->t.set_swz ? v->t.swz : 0; k.stencil = v->t.stencil;
    VkImageView iv = (VkImageView)nvn_hm_get(&g_views, &k); if (iv) return iv;
    unsigned level = has && vw->set_levels ? vw->base_level : 0, nlev = has && vw->set_levels && vw->nlevels ? vw->nlevels : v->levels - level;
    unsigned layer = has && vw->set_layers ? vw->min_layer : 0, nl = has && vw->set_layers && vw->nlayers ? vw->nlayers : (v->type == VK_IMAGE_TYPE_3D ? 1 : v->layers - layer);
    int target = has && vw->set_target ? vw->target : v->t.d.target;
    VkImageViewType vt = view_type(v, target, nl);
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE && nl < 6) || (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && nl % 6)) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) nl = 6;
    VkImageAspectFlags asp = v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (v->t.stencil || (has && vw->set_dsmode && vw->dsmode == 1)) && (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkComponentMapping cm = {v->f.swz[0], v->f.swz[1], v->f.swz[2], v->f.swz[3]};
    if (v->decoded || v->f.expand) cm = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, v->f.swz[3]};
    uint16_t sw = has && vw->set_swizzle ? (uint16_t)(vw->swz[0] | vw->swz[1] << 3 | vw->swz[2] << 6 | vw->swz[3] << 9) : v->t.set_swz ? v->t.swz : 0;
    if (sw || (has && vw->set_swizzle)) {   // the game's swizzle (r | g << 3 | b << 6 | a << 9, NVN's channel enum) picks components of what the format gives
        VkComponentSwizzle inner[4] = {cm.r, cm.g, cm.b, cm.a}, o[4];
        for (int c = 0; c < 4; c++) o[c] = compose(nvn_swz(sw >> (3 * c) & 7), inner, c);
        cm = (VkComponentMapping){o[0], o[1], o[2], o[3]};
    }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->img, vt, v->fmt, cm, {asp, level, nlev ? nlev : 1, layer, nl ? nl : 1}};
    VKC(vkCreateImageView(g_dev, &ci, NULL, &iv)); nvn_hm_put(&g_views, &k, (void *)iv); return iv;
}

// ---------- Replay ----------
static struct { p_set_rt rt; int has_rt; p_scissor sc; int has_sc; int64_t copy_stride, copy_img_stride; } S;
static void view_range(const vtex *v, const view_t *vw, int has, unsigned *level, unsigned *layer, unsigned *nlayers) {
    *level = has && vw->set_levels ? vw->base_level : 0;
    *layer = has && vw->set_layers ? vw->min_layer : 0;
    *nlayers = has && vw->set_layers && vw->nlayers ? vw->nlayers : v->type == VK_IMAGE_TYPE_3D ? 1 : v->layers - *layer;
}
static int can_blit(VkFormat src, VkFormat dst) {   // a blit needs these features of both formats (compressed formats have none)
    VkFormatProperties s, d; vkGetPhysicalDeviceFormatProperties(g_pd, src, &s); vkGetPhysicalDeviceFormatProperties(g_pd, dst, &d);
    return (s.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (d.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}
static int is_layered(const vtex *v) { return v->type != VK_IMAGE_TYPE_3D && (v->layers > 1 || v->t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY || v->t.d.target == NVN_TEXTURE_TARGET_CUBEMAP || v->t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY || v->t.d.target == NVN_TEXTURE_TARGET_1D_ARRAY); }
static VkRect2D scissor_rect(void) { return S.has_sc ? (VkRect2D){{S.sc.x, S.sc.y}, {(uint32_t)(S.sc.w > 0 ? S.sc.w : 0), (uint32_t)(S.sc.h > 0 ? S.sc.h : 0)}} : (VkRect2D){{0, 0}, {1u << 30, 1u << 30}}; }
static void op_clear_color(const p_clear_color *c) {
    if (!S.has_rt || c->index < 0 || c->index >= S.rt.n || !S.rt.color[c->index] || !(c->mask & 0xf)) return;
    vtex *v = tex_use(S.rt.color[c->index], 1); if (!v) return;
    if ((c->mask & 0xf) != 0xf && g_red_mask++ < 4) VLOG("[RED] colour clear with channel mask %#x: all channels cleared (stage 1 has no masked clears)", c->mask);
    unsigned level, layer, n; view_range(v, &S.rt.cview[c->index], S.rt.has_cview[c->index], &level, &layer, &n);
    VkClearValue cv; memcpy(cv.color.float32, c->rgba, 16);
    // an integer target takes the values as integers (the float record holds them exactly up to 2^24)
    VkFormat f = v->fmt;
    if ((f >= VK_FORMAT_R8_UINT && f <= VK_FORMAT_R8_SINT) || (f >= VK_FORMAT_R8G8_UINT && f <= VK_FORMAT_R8G8_SINT) || (f >= VK_FORMAT_R8G8B8A8_UINT && f <= VK_FORMAT_R8G8B8A8_SINT) ||
        (f >= VK_FORMAT_R16_UINT && f <= VK_FORMAT_R16_SINT) || (f >= VK_FORMAT_R16G16_UINT && f <= VK_FORMAT_R16G16_SINT) || (f >= VK_FORMAT_R16G16B16A16_UINT && f <= VK_FORMAT_R16G16B16A16_SINT) ||
        (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SINT) || (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SINT) || (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SINT) ||
        f == VK_FORMAT_A2B10G10R10_UINT_PACK32)
        for (int i = 0; i < 4; i++) cv.color.int32[i] = (int32_t)c->rgba[i];
    clear_rect(v, level, layer, n, scissor_rect(), &cv, VK_IMAGE_ASPECT_COLOR_BIT);
}
static void op_clear_ds(const p_clear_ds *c) {
    if (!S.has_rt || !S.rt.depth) return;
    vtex *v = tex_use(S.rt.depth, 1); if (!v) return;
    VkImageAspectFlags a = (c->depth_mask ? v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c->stencil_mask ? v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!a) return;
    unsigned level, layer, n; view_range(v, &S.rt.dview, S.rt.has_dview, &level, &layer, &n);
    VkClearValue cv; cv.depthStencil.depth = c->depth < 0 ? 0 : c->depth > 1 ? 1 : c->depth; cv.depthStencil.stencil = (uint32_t)c->stencil & 0xff;
    clear_rect(v, level, layer, n, scissor_rect(), &cv, a);
}
static void op_clear_tex(const p_clear_tex *c) {
    vtex *v = tex_use(c->tex, 1); if (!v || !(c->mask & 0xf)) return;
    unsigned level, layer, n; view_range(v, &c->view, c->has_view, &level, &layer, &n);
    if (is_layered(v)) { layer += (unsigned)c->r.zoffset; n = c->r.depth > 0 ? (unsigned)c->r.depth : 1; }
    VkClearValue cv; memcpy(&cv.color, &c->c, 16);
    clear_rect(v, level, layer, n, (VkRect2D){{c->r.xoffset, c->r.yoffset}, {(uint32_t)c->r.width, (uint32_t)c->r.height}}, &cv, VK_IMAGE_ASPECT_COLOR_BIT);
}
static void op_copy_b2t(const p_copy_b2t *c) {
    vtex *v = tex_use(c->tex, 1); if (!v) return;
    int bw, bh, bpb = nvn_fmt_bpb(v->t.d.format, &bw, &bh); if (!bpb || !c->src) return;
    unsigned level, layer, n; view_range(v, &c->view, c->has_view, &level, &layer, &n);
    const NVNcopyRegion *r = &c->r;
    uint32_t bxw = ((uint32_t)r->width + (uint32_t)bw - 1) / (uint32_t)bw, brows = ((uint32_t)r->height + (uint32_t)bh - 1) / (uint32_t)bh, slices = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t stride = S.copy_stride > 0 ? (size_t)S.copy_stride : (size_t)bxw * bpb, istride = S.copy_img_stride > 0 ? (size_t)S.copy_img_stride : stride * brows;
    uint32_t row_len, img_h; stage_t s = convert(v, (const uint8_t *)(uintptr_t)c->src, bxw, brows, slices, stride, istride, &row_len, &img_h);   // the source as it is now
    int layered = is_layered(v);
    VkBufferImageCopy bc = {s.off, row_len, img_h, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, layer + (layered ? (uint32_t)r->zoffset : 0), layered ? slices : 1},
                            {r->xoffset, r->yoffset, layered ? 0 : r->zoffset}, {(uint32_t)r->width, (uint32_t)r->height, layered ? 1 : slices}};
    vkCmdCopyBufferToImage(g_cb, s.buf, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    barrier();
}
static void op_copy_t2b(const p_copy_b2t *c) {
    vtex *v = tex_use(c->tex, 0); if (!v || !c->src) return;
    if (v->decoded || v->f.expand) { static int n; if (n++ < 4) VLOG("[RED] texture-to-buffer copy from a converted texture ser=%llu (format %#x): skipped", (unsigned long long)v->serial, v->t.d.format); return; }
    int bw, bh, bpb = nvn_fmt_bpb(v->t.d.format, &bw, &bh); if (!bpb) return;
    unsigned level, layer, n; view_range(v, &c->view, c->has_view, &level, &layer, &n);
    const NVNcopyRegion *r = &c->r;
    uint32_t bxw = ((uint32_t)r->width + (uint32_t)bw - 1) / (uint32_t)bw, brows = ((uint32_t)r->height + (uint32_t)bh - 1) / (uint32_t)bh, slices = r->depth > 0 ? (uint32_t)r->depth : 1;
    uint32_t rb = bxw * (uint32_t)bpb; stage_t s = stage((VkDeviceSize)rb * brows * slices);
    int layered = is_layered(v);
    VkBufferImageCopy bc = {s.off, 0, 0, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, layer + (layered ? (uint32_t)r->zoffset : 0), layered ? slices : 1},
                            {r->xoffset, r->yoffset, layered ? 0 : r->zoffset}, {(uint32_t)r->width, (uint32_t)r->height, layered ? 1 : slices}};
    vkCmdCopyImageToBuffer(g_cb, v->img, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &bc);
    barrier();
    size_t stride = S.copy_stride > 0 ? (size_t)S.copy_stride : rb, istride = S.copy_img_stride > 0 ? (size_t)S.copy_img_stride : stride * brows;
    post_add((post_t){3, NULL, 0, 0, c->src, 0, s.p, brows, rb, (uint32_t)stride, slices, istride});
}
static void op_copy_t2t(const p_copy_t2t *c) {
    vtex *s = tex_use(c->src, 0), *d = tex_use(c->dst, 1); if (!s || !d) return;
    unsigned sl, sa, sn, dl, da, dn; view_range(s, &c->sv, c->has_sv, &sl, &sa, &sn); view_range(d, &c->dv, c->has_dv, &dl, &da, &dn);
    const NVNcopyRegion *sr = &c->sr, *dr = &c->dr;
    int sly = is_layered(s), dly = is_layered(d);
    uint32_t sd = sr->depth > 0 ? (uint32_t)sr->depth : 1, ddp = dr->depth > 0 ? (uint32_t)dr->depth : 1;
    VkImageSubresourceLayers ss = {s->aspect, sl, sa + (sly ? (uint32_t)sr->zoffset : 0), sly ? sd : 1}, ds = {d->aspect, dl, da + (dly ? (uint32_t)dr->zoffset : 0), dly ? ddp : 1};
    if (c->flags & DM_COPY_RESOLVE) {
        VkImageResolve rv = {ss, {sr->xoffset, sr->yoffset, 0}, ds, {dr->xoffset, dr->yoffset, 0}, {(uint32_t)dr->width, (uint32_t)dr->height, 1}};
        if (s->samples > 1 && d->samples == 1) vkCmdResolveImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
    } else if (sr->width == dr->width && sr->height == dr->height && sd == ddp && s->fmt == d->fmt && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT))) {
        VkImageCopy ic = {ss, {sr->xoffset, sr->yoffset, sly ? 0 : sr->zoffset}, ds, {dr->xoffset, dr->yoffset, dly ? 0 : dr->zoffset}, {(uint32_t)sr->width, (uint32_t)sr->height, sly ? 1 : sd}};
        vkCmdCopyImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {   // scaling, mirroring or a format conversion: a blit (the depth formats must match for it)
        if (s->aspect != d->aspect || (s->aspect != VK_IMAGE_ASPECT_COLOR_BIT && s->fmt != d->fmt) || s->samples > 1 || d->samples > 1 || !can_blit(s->fmt, d->fmt)) { static int n; if (n++ < 4) VLOG("[RED] texture copy %#x → %#x needs a blit Vulkan cannot do: skipped", s->t.d.format, d->t.d.format); return; }
        VkImageBlit b = {ss, {{sr->xoffset, sr->yoffset, sly ? 0 : sr->zoffset}, {sr->xoffset + sr->width, sr->yoffset + sr->height, sly ? 1 : sr->zoffset + (int)sd}},
                         ds, {{dr->xoffset, dr->yoffset, dly ? 0 : dr->zoffset}, {dr->xoffset + dr->width, dr->yoffset + dr->height, dly ? 1 : dr->zoffset + (int)ddp}}};
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t t = b.dstOffsets[0].x; b.dstOffsets[0].x = b.dstOffsets[1].x; b.dstOffsets[1].x = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t t = b.dstOffsets[0].y; b.dstOffsets[0].y = b.dstOffsets[1].y; b.dstOffsets[1].y = t; }
        vkCmdBlitImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && s->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier();
}
static void cpu_buffer_op(void) { if (g_readbacks) flush(); }   // a buffer write after a pending readback: the readback lands first
static void exec_list(const uint8_t *p, int depth) {
    for (int guard = 0; guard < 1 << 24; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth < 16) exec_list((const uint8_t *)(uintptr_t)t, depth + 1); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); post_add((post_t){1, val, m, v, 0, 0, NULL, 0, 0, 0, 0, 0}); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); post_add((post_t){2, NULL, 0, 0, ad, t, NULL, 0, 0, 0, 0, 0}); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { cpu_buffer_op(); const p_clear_buf *cb = (const p_clear_buf *)a; uint32_t *d = (uint32_t *)(uintptr_t)cb->addr; for (uint64_t i = 0; i < cb->size / 4; i++) d[i] = cb->value; break; }
            case OP_COPY_B2B: { cpu_buffer_op(); uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); memmove((void *)(uintptr_t)d, (const void *)(uintptr_t)s_, n); break; }
            case OP_UPDATE_UB: { cpu_buffer_op(); uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)(uintptr_t)d, a + 16, n); break; }
            case OP_SET_RT: memcpy(&S.rt, a, sizeof S.rt); S.has_rt = 1; break;
            case OP_SCISSOR: memcpy(&S.sc, a, sizeof S.sc); S.has_sc = 1; break;
            case OP_COPY_STRIDE: { const p_copy_stride *cs = (const p_copy_stride *)a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
            case OP_CLEAR_COLOR: op_clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: op_clear_ds((const p_clear_ds *)a); break;
            case OP_CLEAR_TEX: op_clear_tex((const p_clear_tex *)a); break;
            case OP_COPY_B2T: op_copy_b2t((const p_copy_b2t *)a); break;
            case OP_COPY_T2B: op_copy_t2b((const p_copy_b2t *)a); break;
            case OP_COPY_T2T: op_copy_t2t((const p_copy_t2t *)a); break;
            case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: if (!g_red_draw++) VLOG("[RED] draws and dispatches are not executed yet (Vulkan backend stage 1): the picture is only clears and copies"); break;
            default: break;
        }
        if (h.size < 8) { VLOG("[RED] command record of size %u at %p (op %u): submission cut", h.size, (const void *)p, h.op); return; }
        p += h.size;
    }
    VLOG("[RED] a command list did not end within 16M records: submission cut");
}
void mtl_sub_begin(void) { lock(); g_sub++; S.has_rt = 0; rec_begin(); }   // the render targets are the snapshots of this submission's records
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_exec(const uint8_t *p) { exec_list(p, 0); }
uint64_t mtl_sub_commit(void) { flush(); uint64_t f = ++g_fence; unlock(); return f; }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }   // every submission completed in its commit
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}

// ---------- Texture lifetime ----------
void mtl_tex_init(const tex_t *t) {   // a serial initialized again with another shape is replaced at its next use (tex_use)
    (void)t;
}
void mtl_tex_release(tex_t *t) {
    if (!t || !t->serial) return;
    lock();
    vtex *v = nvn_hm_del(&g_tex, &t->serial);
    if (v) { flush(); tex_free(v); }
    unlock();
}
void mtl_tex_cpu_written(const tex_t *t) {
    if (!t || !t->serial) return;
    lock();
    vtex *v = nvn_hm_get(&g_tex, &t->serial);
    if (v) { v->cpu_written = 1; v->gpu_owned = 0; }
    else { v = tex_make(t); if (v) { v->cpu_written = 1; nvn_hm_put(&g_tex, &t->serial, v); } }
    unlock();
}
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    lock(); rec_begin();
    vtex *v = tex_use(t, 0);
    int bw, bh, bpb = v ? nvn_fmt_bpb(v->t.d.format, &bw, &bh) : 0;
    if (!v || !bpb || v->decoded || v->f.expand) {   // nothing to read: zeros, as the null device
        size_t rows = (size_t)(r->height > 0 ? r->height : 1), depth = (size_t)(r->depth > 0 ? r->depth : 1);
        for (size_t z = 0; z < depth; z++) for (size_t y = 0; y < rows; y++) memset(dst + z * img + y * row, 0, row);
        unlock(); return;
    }
    uint32_t bxw = ((uint32_t)r->width + (uint32_t)bw - 1) / (uint32_t)bw, brows = ((uint32_t)r->height + (uint32_t)bh - 1) / (uint32_t)bh, slices = r->depth > 0 ? (uint32_t)r->depth : 1;
    uint32_t rb = bxw * (uint32_t)bpb; stage_t s = stage((VkDeviceSize)rb * brows * slices);
    int layered = is_layered(v);
    VkBufferImageCopy bc = {s.off, 0, 0, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, layer0 + (layered ? (uint32_t)r->zoffset : 0), layered ? slices : 1},
                            {r->xoffset, r->yoffset, layered ? 0 : r->zoffset}, {(uint32_t)r->width, (uint32_t)r->height, layered ? 1 : slices}};
    vkCmdCopyImageToBuffer(g_cb, v->img, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &bc);
    post_add((post_t){3, NULL, 0, 0, (uint64_t)(uintptr_t)dst, 0, s.p, brows, rb, (uint32_t)row, slices, img});
    flush();
    unlock();
}
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
static atomic_int g_depth_zero, g_origin_ll = 1;   // NVN starts with the GL conventions (lower-left origin)
void mtl_set_depth_zero(int on) { atomic_store(&g_depth_zero, on); }
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) VLOG("window origin: %s", on ? "lower left (presents flipped)" : "upper left"); }

// ---------- Memory pools ----------
#define NPOOL 4096
static struct { uint64_t base, size; } g_pool[NPOOL]; static int g_npool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool < NPOOL) { g_pool[g_npool].base = (uint64_t)(uintptr_t)mem; g_pool[g_npool].size = size; g_npool++; }
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

// ---------- Presentation ----------
static void *g_win; static VkSurfaceKHR g_surf; static VkSwapchainKHR g_sc; static VkImage g_scimg[8]; static uint32_t g_scn; static VkExtent2D g_scext; static VkFormat g_scfmt;
static int g_sc_srgb, g_sc_bad;
enum { NFRAME = 3 };
static VkCommandBuffer g_pcb[NFRAME]; static VkFence g_pfence[NFRAME]; static VkSemaphore g_acq[NFRAME], g_rel[8]; static unsigned g_pframe;
static uint64_t g_ticket;
static void present_objs(void) {
    if (g_pcb[0]) return;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, NFRAME};
    VKC(vkAllocateCommandBuffers(g_dev, &cai, g_pcb));
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < NFRAME; i++) { VKC(vkCreateFence(g_dev, &fi, NULL, &g_pfence[i])); VKC(vkCreateSemaphore(g_dev, &si, NULL, &g_acq[i])); }
    for (int i = 0; i < 8; i++) VKC(vkCreateSemaphore(g_dev, &si, NULL, &g_rel[i]));
}
static void swapchain_drop(int surface) {
    vkQueueWaitIdle(g_q);
    if (g_sc) { vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; g_scn = 0; }
    if (surface && g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; g_win = NULL; }
}
// Called by the front end when the window goes away (front_android.c APP_CMD_TERM_WINDOW), before it may be released: nothing presents to it afterwards
void nvn_vk_window_lost(void) { if (!g_dev) return; lock(); swapchain_drop(1); unlock(); }
static int swapchain_make(int srgb) {
    VkSurfaceCapabilitiesKHR cap; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap));
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &ok); if (!ok) { VLOG("[RED] the graphics queue cannot present to the window"); return 0; }
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fs[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fs);
    VkFormat want[2] = {srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM};
    VkSurfaceFormatKHR sf = fs[0];
    for (int w = 1; w >= 0; w--) for (uint32_t i = 0; i < nf; i++) if (fs[i].format == want[w]) sf = fs[i];
    uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, g_surf, &npm, NULL);
    VkPresentModeKHR pms[8]; if (npm > 8) npm = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, g_surf, &npm, pms);
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR; for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) pm = pms[i];
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xffffffffu) { ext.width = 1280; ext.height = 720; }
    if (!ext.width || !ext.height) return 0;
    uint32_t n = cap.minImageCount + 1; if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount; if (n > 8) n = 8;
    VkCompositeAlphaFlagBitsKHR ca = cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, g_surf, n, sf.format, sf.colorSpace, ext, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                   cap.currentTransform, ca, pm, VK_TRUE, g_sc};
    VkSwapchainKHR sc; VkResult r = vkCreateSwapchainKHR(g_dev, &ci, NULL, &sc);
    if (g_sc) vkDestroySwapchainKHR(g_dev, g_sc, NULL);
    g_sc = VK_NULL_HANDLE; if (r < 0) { VLOG("[RED] vkCreateSwapchainKHR: %d", (int)r); return 0; }
    g_sc = sc; g_scext = ext; g_scfmt = sf.format; g_sc_srgb = srgb;
    g_scn = 8; vkGetSwapchainImagesKHR(g_dev, g_sc, &g_scn, g_scimg);
    VLOG("swapchain %ux%u, %u images, format %d, %s%s", ext.width, ext.height, g_scn, (int)sf.format, pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
         cap.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? " (pre-rotated by the compositor)" : "");
    return 1;
}
static int is_srgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK; }
// The destination rectangle of a cw × ch picture fitted into dw × dh with its aspect kept (centred)
static void fit(uint32_t cw, uint32_t ch, uint32_t dw, uint32_t dh, VkOffset3D o[2]) {
    double s = (double)dw / cw < (double)dh / ch ? (double)dw / cw : (double)dh / ch;
    int w = (int)(cw * s + 0.5), h = (int)(ch * s + 0.5), x = ((int)dw - w) / 2, y = ((int)dh - h) / 2;
    o[0] = (VkOffset3D){x, y, 0}; o[1] = (VkOffset3D){x + w, y + h, 1};
}
// Without a window (tests): the frame blitted into this host-visible copy, cw × ch RGBA8
static VkImage g_tframe; static vmem g_tframe_m; static uint32_t g_tw, g_th; static VkBuffer g_tbuf; static vmem g_tbuf_m; static int g_thave;
int nvn_vk_test_frame(uint8_t *rgba, uint32_t w, uint32_t h) {
    lock(); int ok = g_thave && w == g_tw && h == g_th; if (ok) memcpy(rgba, g_tbuf_m.map, (size_t)w * h * 4); unlock(); return ok;
}
static void present_headless(vtex *v, VkOffset3D src[2], uint32_t cw, uint32_t ch) {
    if (!g_tframe || g_tw != cw || g_th != ch) {
        if (g_tframe) { vkDestroyImage(g_dev, g_tframe, NULL); mem_free(&g_tframe_m); vkDestroyBuffer(g_dev, g_tbuf, NULL); mem_free(&g_tbuf_m); }
        g_tw = cw; g_th = ch;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {cw, ch, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VKC(vkCreateImage(g_dev, &ci, NULL, &g_tframe)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(g_dev, g_tframe, &rq);
        g_tframe_m = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); VKC(vkBindImageMemory(g_dev, g_tframe, g_tframe_m.mem, g_tframe_m.off));
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)cw * ch * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VKC(vkCreateBuffer(g_dev, &bi, NULL, &g_tbuf)); vkGetBufferMemoryRequirements(g_dev, g_tbuf, &rq);
        g_tbuf_m = mem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1); VKC(vkBindBufferMemory(g_dev, g_tbuf, g_tbuf_m.mem, g_tbuf_m.off));
    }
    rec_begin();
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_tframe, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {src[0], src[1]}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)cw, (int)ch, 1}}};
    if (atomic_load(&g_origin_ll)) { b.dstOffsets[0].y = (int)ch; b.dstOffsets[1].y = 0; }
    vkCmdBlitImage(g_cb, v->img, VK_IMAGE_LAYOUT_GENERAL, g_tframe, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    barrier();
    VkBufferImageCopy bc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {cw, ch, 1}};
    vkCmdCopyImageToBuffer(g_cb, g_tframe, VK_IMAGE_LAYOUT_GENERAL, g_tbuf, 1, &bc);
    flush(); g_thave = 1;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    lock();
    uint64_t ticket = ++g_ticket;
    vtex *v = tex_use(t, 0);
    if (!v || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || v->samples > 1) { unlock(); return ticket; }
    if (!can_blit(v->fmt, VK_FORMAT_R8G8B8A8_UNORM)) { static int n; if (n++ < 4) VLOG("[RED] present of texture ser=%llu: its format (Vulkan %d) cannot be blitted; not shown", (unsigned long long)v->serial, (int)v->fmt); unlock(); return ticket; }
    int cx = 0, cy = 0, cw = (int)v->w, ch = (int)v->h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    VkOffset3D src[2] = {{cx, cy, 0}, {cx + cw, cy + ch, 1}};
    void *win = front_metal_layer();
    if (!win) { present_headless(v, src, (uint32_t)cw, (uint32_t)ch); unlock(); return ticket; }
#ifdef __ANDROID__
    present_objs();
    if (win != g_win) {   // a new window: its surface and swapchain
        swapchain_drop(1);
        VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, (ANativeWindow *)win};
        PFN_vkCreateAndroidSurfaceKHR cs = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(g_inst, "vkCreateAndroidSurfaceKHR");
        if (!cs || cs(g_inst, &si, NULL, &g_surf) < 0) { VLOG("[RED] cannot make a Vulkan surface on the window"); unlock(); return ticket; }
        g_win = win;
    }
    int srgb = is_srgb(v->fmt);
    if ((!g_sc || g_sc_bad || srgb != g_sc_srgb) && !swapchain_make(srgb)) { unlock(); return ticket; }
    g_sc_bad = 0;
    unsigned fi = g_pframe++ % NFRAME;
    VKC(vkWaitForFences(g_dev, 1, &g_pfence[fi], VK_TRUE, UINT64_MAX));
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 1000000000ull, g_acq[fi], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT || r == VK_NOT_READY) { g_sc_bad = 1; unlock(); return ticket; }
    if (r < 0) { VLOG("[RED] vkAcquireNextImageKHR: %d", (int)r); g_sc_bad = 1; unlock(); return ticket; }
    VKC(vkResetFences(g_dev, 1, &g_pfence[fi]));
    VkCommandBuffer cb = g_pcb[fi]; VKC(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKC(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_scimg[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange one = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_scimg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &one);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {src[0], src[1]}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0}}};
    fit((uint32_t)cw, (uint32_t)ch, g_scext.width, g_scext.height, b.dstOffsets);
    if (atomic_load(&g_origin_ll)) { int32_t y = b.dstOffsets[0].y; b.dstOffsets[0].y = b.dstOffsets[1].y; b.dstOffsets[1].y = y; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, v->fmt, &fp);
    vkCmdBlitImage(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, g_scimg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = 0; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VKC(vkEndCommandBuffer(cb));
    // the texture's own pending work (its uploads) goes first, on the same queue
    flush();
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &g_acq[fi], &ws, 1, &cb, 1, &g_rel[idx]};
    VKC(vkQueueSubmit(g_q, 1, &si, g_pfence[fi]));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &g_rel[idx], 1, &g_sc, &idx, NULL};
    r = vkQueuePresentKHR(g_q, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_sc_bad = 1;
    else if (r < 0) VLOG("[RED] vkQueuePresentKHR: %d", (int)r);
#else
    (void)present_objs; (void)swapchain_make; (void)fit; (void)g_sc_bad; (void)g_pframe;
#endif
    unlock();
    return ticket;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_display_hz(void) { return front_metal_layer() ? 60 : 0; }
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

// ---------- Diagnostics ----------
void dm_gfx_shot_at(long k, const char *path) { (void)k; VLOG("[RED] no screenshot of %s yet on Vulkan", path); }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) {
    VkDeviceSize used = 0; for (int i = 0; i < g_nblk; i++) if (g_blk[i].mem) { used += g_blk[i].size; for (int k = 0; k < g_blk[i].nfree; k++) used -= g_blk[i].free[k].size; }
    VLOG("memory at frame %llu: %llu MB resident; GPU blocks %d, %llu MB in use; textures %u", frame, mb, g_nblk, (unsigned long long)(used >> 20), g_tex.n);
}
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "textures %u", g_tex.n); }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_ubp_why(uint64_t w[5]) { for (int i = 0; i < 5; i++) w[i] = 0; }
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
