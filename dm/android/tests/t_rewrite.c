// End-to-end check of rewritten guest code on bionic: a module rewritten with DM_TARGET=android (run.sh makes it from the synthetic NSO of
// dm/tools/tests/test_rewrite_target.py) is mapped as dm/host/loader.c maps it (copy, key page of measured bionic_tls offsets, instruction cache, r-x code)
// and its function is called on two threads with the thread slots set as dm/host/thread.c sets them. Its code (mrs x0, tpidr_el0; mrs x1, tpidrro_el0;
// mov x18, x0; mov x2, x18; svc #1; ret) must reach the gate (dm/host/entry.S converted to ELF) with x0 = the thread's TLS slot, x1 = its thread-local
// region slot, x2 = x0 through the x18 slot, leave the guest's x18 in its slot, and return what the gate put in x0.
// Usage: t_rewrite <dir with main.patched.bin, main.fixups.tsv, main.gates.tsv>
#include "../compat/darwin_compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail++; } printf("\n"); } while (0)

pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
extern void dm_gate(void);
static uint8_t *g_base; static int64_t g_patch_off; static uint64_t g_text_end, g_key_page_off, g_size; static unsigned g_key_idx[4];
static uint64_t g_gate_ret; static unsigned g_gate_svc;

// What entry.S calls
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
static __thread uint64_t t_seen[4]; static __thread int t_hits;
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) {
    t_hits++;
    if (ret_addr - (uint64_t)g_base != g_gate_ret) { printf("FAIL gate return point %#llx, gates.tsv says %#llx\n", (unsigned long long)(ret_addr - (uint64_t)g_base), (unsigned long long)g_gate_ret); g_fail++; }
    for (int i = 0; i < 3; i++) t_seen[i] = regs[i];
    regs[0] = 0x5a5a0000u | g_gate_svc; return regs[0];
}
void unimpl_c(uint64_t idx, uint64_t *regs) { (void)regs; printf("FAIL unimplemented stub %llu called\n", (unsigned long long)idx); g_fail++; }
void dm_thread_returned(void) { printf("FAIL dm_thread_returned\n"); g_fail++; }

static void load(const char *dir) {
    char p[1024]; snprintf(p, sizeof p, "%s/main.fixups.tsv", dir);
    FILE *f = fopen(p, "r"); if (!f) { perror(p); exit(2); }
    char line[256], key[32]; unsigned long v; int target = 0;
    while (fgets(line, sizeof line, f)) {
        char tg[16]; if (sscanf(line, "# TARGET %15s", tg) == 1) { target = !strcmp(tg, "android"); continue; }
        if (sscanf(line, "# %31s %lx", key, &v) != 2) continue;
        if (!strcmp(key, "PATCH_OFF")) g_patch_off = strtoll(line + 2 + strlen(key), NULL, 16);
        else if (!strcmp(key, "KEY_PAGE_OFF")) g_key_page_off = v;
        else if (!strcmp(key, "TEXT_END")) g_text_end = v;
        else for (int k = 0; k < 4; k++) { static const char *kn[4] = {"KEY_K18_IDX", "KEY_KTLS_IDX", "KEY_KTLR_IDX", "KEY_KGATE_IDX"}; if (!strcmp(key, kn[k])) g_key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); }
    }
    fclose(f);
    CHECK(target, "module rewritten for Android (fixups TARGET)");
    snprintf(p, sizeof p, "%s/main.gates.tsv", dir); f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) { unsigned long long r; char kind[16]; unsigned arg; if (line[0] != '#' && sscanf(line, "%llx %15s %u", &r, kind, &arg) == 3) { g_gate_ret = r; g_gate_svc = arg; } }
    if (f) fclose(f);
    CHECK(g_gate_ret, "gates.tsv has the svc gate (return point %#llx, svc %#x)", (unsigned long long)g_gate_ret, g_gate_svc);
    snprintf(p, sizeof p, "%s/main.patched.bin", dir); f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    // As loader.c map_image (patch area after the image): anonymous pages, copy, key page, then the code made r-x
    g_size = ((uint64_t)sz + 0x3FFF) & ~0x3FFFull;
    g_base = mmap(NULL, g_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fread(g_base, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); exit(2); } fclose(f);
    pthread_key_t *keys[4] = {&g_key18, &g_keytls, &g_keytlr, &g_keygate};
    uint64_t *page = (uint64_t *)(g_base + g_key_page_off);
    for (int k = 0; k < 4; k++) { pthread_key_create(keys[k], NULL); page[g_key_idx[k]] = dm_bionic_key_offset(*keys[k]); }
    mprotect(page, 0x4000, PROT_READ);
    uint64_t rx = (g_text_end + 0x3FFF) & ~0x3FFFull, po = (uint64_t)g_patch_off & ~0x3FFFull;
    __builtin___clear_cache((char *)g_base, (char *)g_base + rx); __builtin___clear_cache((char *)g_base + po, (char *)g_base + g_size);
    CHECK(!mprotect(g_base, rx, PROT_READ | PROT_EXEC) && !mprotect(g_base + po, g_size - po, PROT_READ | PROT_EXEC), "code mapped r-x");
}
static void *run(void *a) {
    uintptr_t tag = (uintptr_t)a;
    // As thread.c guest_thread_init: the gate slot, x18 cleared, the TLS block and the thread-local region (distinct values per thread here)
    pthread_setspecific(g_keygate, (void *)dm_gate); pthread_setspecific(g_key18, 0);
    pthread_setspecific(g_keytls, (void *)(0x71500000 + tag)); pthread_setspecific(g_keytlr, (void *)(0x71600000 + tag));
    uint64_t (*fn)(void) = (uint64_t (*)(void))(g_base + 8);
    uint64_t r = fn();
    int ok = t_hits == 1 && t_seen[0] == 0x71500000 + tag && t_seen[1] == 0x71600000 + tag && t_seen[2] == 0x71500000 + tag
             && (uint64_t)pthread_getspecific(g_key18) == 0x71500000 + tag && r == (0x5a5a0000u | g_gate_svc);
    if (!ok) printf("     thread %lu: gate hits %d, x0 %#llx x1 %#llx x2 %#llx, x18 slot %#llx, returned %#llx\n", (unsigned long)tag, t_hits, (unsigned long long)t_seen[0],
                    (unsigned long long)t_seen[1], (unsigned long long)t_seen[2], (unsigned long long)(uint64_t)pthread_getspecific(g_key18), (unsigned long long)r);
    return (void *)(intptr_t)ok;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: t_rewrite <dir>\n"); return 2; }
    load(argv[1]);
    register uint64_t host_x18 __asm__("x18"); uint64_t x18_before; __asm__ volatile("mov %0, x18" : "=r"(x18_before)); (void)host_x18;
    CHECK((intptr_t)run((void *)1) == 1, "main thread: tpidr_el0 / tpidrro_el0 / x18 reach their slots and the svc reaches the gate");
    uint64_t x18_after; __asm__ volatile("mov %0, x18" : "=r"(x18_after));
    CHECK(x18_before == x18_after, "the host's own x18 is untouched (%#llx)", (unsigned long long)x18_after);
    pthread_t t[4]; int ok = 1;
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, run, (void *)(uintptr_t)(i + 2));
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); ok &= (intptr_t)r == 1; }
    CHECK(ok, "4 more threads: each sees its own slots");
    printf("%d failed\n", g_fail);
    return g_fail;
}
