// Tests of dm/android/compat on bionic: a static arm64 executable (dm/android/tests/run.sh builds it with the NDK and runs it on a device through adb, or under
// qemu-aarch64 when there is none). Each check prints ok / FAIL; the exit code is the number of failures.
//   thread slots: the key page offset the loader measures (dm_bionic_key_offset) reaches each thread's own key value through tpidr_el0 → slot 1, as the rewritten code does
//   thread_suspend / thread_get_state / thread_resume on a running thread; thread_info; task_threads
//   vm_read_overwrite on readable and unreadable memory; vm_region_64; signal context records (FPSIMD, ESR)
//   SHA-256; the CFDictionary stand-in; dispatch semaphores; clonefile / renamex_np / xattrs; registry sections (getsectiondata)
#include "../compat/darwin_compat.h"
#include <CoreFoundation/CoreFoundation.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail++; } printf("\n"); } while (0)

// ---- thread slots ----
static pthread_key_t g_keys[4];
static uint64_t g_off[4];
static uint64_t read_slot(uint64_t off) {   // the rewritten code's sequence: mrs tpidr_el0; ldur [,#-8]; add the key page entry; ldr
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0\n ldur %0, [%0, #-8]\n add %0, %0, %1\n ldr %0, [%0]" : "=&r"(v) : "r"(off));
    return v;
}
static void *slot_thread(void *a) {
    uintptr_t base = (uintptr_t)a; int ok = 1;
    for (int i = 0; i < 4; i++) pthread_setspecific(g_keys[i], (void *)(base + (uintptr_t)i));
    for (int i = 0; i < 4; i++) ok &= read_slot(g_off[i]) == base + (uint64_t)i;
    return (void *)(intptr_t)ok;
}
static void test_slots(void) {
    pthread_key_t pad[7]; for (int i = 0; i < 7; i++) pthread_key_create(&pad[i], NULL);   // keys taken before the host's, as libraries do
    for (int i = 0; i < 4; i++) pthread_key_create(&g_keys[i], NULL);
    int found = 1; for (int i = 0; i < 4; i++) { g_off[i] = dm_bionic_key_offset(g_keys[i]); found &= g_off[i] != ~0ull; }
    CHECK(found, "thread slots: key offsets measured (+%#llx +%#llx +%#llx +%#llx)", (unsigned long long)g_off[0], (unsigned long long)g_off[1], (unsigned long long)g_off[2], (unsigned long long)g_off[3]);
    if (!found) return;
    int ok = 1; pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, slot_thread, (void *)(uintptr_t)(0x1000 * (i + 1)));
    for (int i = 0; i < 8; i++) { void *r; pthread_join(t[i], &r); ok &= (intptr_t)r == 1; }
    ok &= (intptr_t)slot_thread((void *)0x99000) == 1;
    CHECK(ok, "thread slots: 9 threads each read their own 4 values through tpidr_el0");
}

// ---- thread suspension ----
static _Atomic int g_spin_tid, g_spin_stop; static volatile uint64_t g_spin_n;   // one writer: a plain increment keeps the loop free of calls
static void *spinner(void *a) {
    (void)a; g_spin_tid = gettid();
    register uint64_t x19 __asm__("x19") = 0x1234567890abcdefull;
    while (!atomic_load(&g_spin_stop)) { __asm__ volatile("" : "+r"(x19)); g_spin_n++; }
    return (void *)x19;
}
static void test_suspend(void) {
    pthread_t t; pthread_create(&t, NULL, spinner, NULL);
    while (!g_spin_tid || g_spin_n < 1000) sched_yield();
    kern_return_t kr = thread_suspend((thread_act_t)g_spin_tid);
    CHECK(kr == KERN_SUCCESS, "thread_suspend a running thread");
    uint64_t n0 = g_spin_n; struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
    CHECK(g_spin_n == n0, "suspended thread makes no progress (%llu → %llu)", (unsigned long long)n0, (unsigned long long)g_spin_n);
    arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
    kr = thread_get_state((thread_act_t)g_spin_tid, ARM_THREAD_STATE64, (thread_state_t)&st, &c);
    CHECK(kr == KERN_SUCCESS && st.__x[19] == 0x1234567890abcdefull, "thread_get_state reads its x19 (%#llx)", (unsigned long long)st.__x[19]);
    CHECK(st.__pc >= (uint64_t)(uintptr_t)spinner && st.__pc < (uint64_t)(uintptr_t)spinner + 256 && st.__sp, "its pc is in the spin loop (pc %#llx, spinner %p)", (unsigned long long)st.__pc, (void *)spinner);
    thread_basic_info_data_t bi; c = THREAD_BASIC_INFO_COUNT;
    CHECK(thread_info((thread_act_t)g_spin_tid, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS && (bi.user_time.seconds || bi.user_time.microseconds), "thread_info gives its CPU time (%d.%06d s)", bi.user_time.seconds, bi.user_time.microseconds);
    CHECK(thread_resume((thread_act_t)g_spin_tid) == KERN_SUCCESS, "thread_resume");
    while (g_spin_n == n0) sched_yield();
    CHECK(1, "resumed thread runs again");
    thread_act_array_t list; mach_msg_type_number_t nl = 0; int seen = 0;
    if (task_threads(mach_task_self(), &list, &nl) == KERN_SUCCESS) { for (unsigned i = 0; i < nl; i++) seen |= (int)list[i] == g_spin_tid; vm_deallocate(mach_task_self(), (vm_address_t)list, nl * sizeof *list); }
    CHECK(seen && nl >= 2, "task_threads lists it (%u threads)", nl);
    g_spin_stop = 1; void *r; pthread_join(t, &r);
    CHECK(thread_suspend((thread_act_t)gettid()) != KERN_SUCCESS, "suspending oneself is refused");
}

// ---- memory ----
static void test_memory(void) {
    uint8_t *m = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(m, 0x5a, 4096); mprotect(m + 4096, 4096, PROT_NONE); mprotect(m + 8192, 4096, PROT_READ | PROT_EXEC);
    uint8_t buf[64]; vm_size_t got = 0;
    CHECK(vm_read_overwrite(0, (vm_address_t)m, 64, (vm_address_t)buf, &got) == KERN_SUCCESS && got == 64 && buf[63] == 0x5a, "vm_read_overwrite readable memory");
    got = 0;
    CHECK(vm_read_overwrite(0, (vm_address_t)(m + 4096), 64, (vm_address_t)buf, &got) != KERN_SUCCESS, "vm_read_overwrite unreadable memory fails without a fault");
    vm_address_t a = (vm_address_t)(m + 8192); vm_size_t sz = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t o;
    kern_return_t kr = vm_region_64(0, &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &o);
    CHECK(kr == KERN_SUCCESS && a == (vm_address_t)(m + 8192) && bi.protection == (VM_PROT_READ | VM_PROT_EXECUTE), "vm_region_64 finds the r-x page (prot %d)", bi.protection);
    a = (vm_address_t)m; kr = vm_region_64(0, &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &c, &o);
    CHECK(kr == KERN_SUCCESS && bi.protection == (VM_PROT_READ | VM_PROT_WRITE), "vm_region_64 finds the rw- page");
    munmap(m, 3 * 4096);
    vm_address_t va = 0; CHECK(vm_allocate(0, &va, 1 << 20, VM_FLAGS_ANYWHERE) == KERN_SUCCESS && va, "vm_allocate");
    CHECK(vm_deallocate(0, va, 1 << 20) == KERN_SUCCESS, "vm_deallocate");
}

// ---- signal context records ----
static volatile uint64_t g_esr; static volatile int g_v0_ok, g_ss_ok; static sigjmp_buf g_jb;
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void)sig; ucontext_t *uc = uc_;
    g_esr = dm_uc_esr((struct ucontext *)uc);
    __uint128_t *v = dm_uc_v((struct ucontext *)uc); g_v0_ok = v && (uint64_t)v[7] == 0x0123456789abcdefull;
    g_ss_ok = ((arm_thread_state64_t *)&uc->uc_mcontext.regs[0])->__pc == uc->uc_mcontext.pc && (uint64_t)si->si_addr != 0;
    siglongjmp(g_jb, 1);
}
static void test_context(void) {
    struct sigaction sa = {0}, old; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, &old);
    uint8_t *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!sigsetjmp(g_jb, 1)) {
        uint64_t v = 0x0123456789abcdefull;
        __asm__ volatile("fmov d7, %0\n strb wzr, [%1]" : : "r"(v), "r"(ro) : "v7", "memory");
    }
    sigaction(SIGSEGV, &old, NULL); munmap(ro, 4096);
    CHECK(g_ss_ok, "integer registers read through the Darwin layout (DM_UC_SS)");
    CHECK(g_v0_ok, "FP / SIMD registers from the FPSIMD record (v7)");
    if (!g_esr && getenv("DM_TEST_QEMU")) printf("skip fault syndrome: qemu writes no ESR record\n");   // the kernel does on a device
    else CHECK(g_esr && (g_esr >> 6 & 1), "fault syndrome from the ESR record, write bit set (ESR %#llx)", (unsigned long long)g_esr);
}

// ---- the rest ----
static void test_misc(void) {
    uint8_t d[32]; CC_SHA256("abc", 3, d);
    static const uint8_t want[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(!memcmp(d, want, 32), "SHA-256 of \"abc\"");
    CC_SHA256_CTX c; CC_SHA256_Init(&c); for (int i = 0; i < 1000; i++) CC_SHA256_Update(&c, "a", 1); uint8_t d2[32]; CC_SHA256_Final(d2, &c);
    static const uint8_t want2[32] = {0x41, 0xed, 0xec, 0xe4, 0x2d, 0x63, 0xe8, 0xd9, 0xbf, 0x51, 0x5a, 0x9b, 0xa6, 0x93, 0x2e, 0x1c, 0x20, 0xcb, 0xc9, 0xf5, 0xa5, 0xd1, 0x34, 0x64, 0x5a, 0xdb, 0x5d, 0xb1, 0xb9, 0x73, 0x7e, 0xa3};
    CHECK(!memcmp(d2, want2, 32), "SHA-256 of 1000 × \"a\" fed a byte at a time");
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(NULL, 0, NULL, NULL); int ok = 1;
    for (uintptr_t i = 1; i <= 20000; i++) CFDictionarySetValue(m, (void *)(i * 16), (void *)i);
    for (uintptr_t i = 1; i <= 20000; i += 2) CFDictionaryRemoveValue(m, (void *)(i * 16));
    for (uintptr_t i = 1; i <= 20000; i++) ok &= CFDictionaryGetValue(m, (void *)(i * 16)) == (i % 2 ? NULL : (void *)i);
    for (uintptr_t i = 1; i <= 20000; i += 2) CFDictionarySetValue(m, (void *)(i * 16), (void *)(i + 1));
    for (uintptr_t i = 1; i <= 20000; i += 2) ok &= CFDictionaryGetValue(m, (void *)(i * 16)) == (void *)(i + 1);
    CHECK(ok && CFDictionaryGetCount(m) == 20000, "CFDictionary: 20,000 set, half removed and set again");
    CFRelease(m);
    dispatch_semaphore_t s = dispatch_semaphore_create(0); uint64_t t0 = mach_absolute_time();
    long r = dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 50000000)); uint64_t dt = mach_absolute_time() - t0;
    CHECK(r != 0 && dt >= 45000000 && dt < 500000000, "dispatch semaphore times out after 50 ms (%.1f ms)", dt / 1e6);
    dispatch_semaphore_signal(s); CHECK(dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER) == 0, "dispatch semaphore signalled"); dispatch_release(s);
}
static void test_files(const char *tmp) {
    char a[512], b[512], f[600], g[600]; snprintf(a, sizeof a, "%s/cf_a", tmp); snprintf(b, sizeof b, "%s/cf_b", tmp);
    snprintf(f, sizeof f, "%s/sub/x.bin", a);
    char sub[600]; snprintf(sub, sizeof sub, "%s/sub", a); mkdir(a, 0755); mkdir(sub, 0755);
    FILE *fp = fopen(f, "w"); fputs("hello", fp); fclose(fp);
    int xa = setxattr(f, "dm.guest", "Name", 4, 0, XATTR_NOFOLLOW);
    CHECK(clonefile(a, b, 0) == 0, "clonefile copies a tree");
    snprintf(g, sizeof g, "%s/sub/x.bin", b); char rd[8] = ""; fp = fopen(g, "r"); if (fp) { if (!fgets(rd, sizeof rd, fp)) rd[0] = 0; fclose(fp); }
    CHECK(!strcmp(rd, "hello"), "the copy has the file's contents");
    if (xa == 0) { char v[16] = ""; ssize_t n = getxattr(g, "dm.guest", v, sizeof v, 0, XATTR_NOFOLLOW); CHECK(n == 4 && !memcmp(v, "Name", 4), "the copy keeps the xattr"); }
    else printf("skip xattrs: %s\n", strerror(errno));
    CHECK(clonefile(a, b, 0) && errno == EEXIST, "clonefile onto an existing path fails with EEXIST");
    fp = fopen(f, "w"); fputs("A", fp); fclose(fp); fp = fopen(g, "w"); fputs("B", fp); fclose(fp);
    CHECK(renamex_np(a, b, RENAME_SWAP) == 0, "renamex_np RENAME_SWAP");
    fp = fopen(f, "r"); rd[0] = 0; if (fp) { if (!fgets(rd, sizeof rd, fp)) rd[0] = 0; fclose(fp); }
    CHECK(!strcmp(rd, "B"), "the two trees changed places");
    char p[64]; int fd = open(f, O_RDONLY); char path[PATH_MAX] = "";
    CHECK(fcntl(fd, F_GETPATH, path) == 0 && !strcmp(path, f), "fcntl F_GETPATH (%s)", path); close(fd); (void)p;
}
__attribute__((used, section("swhle"))) static const struct { const char *n; void *f; } t_ent[2] = {{"one", (void *)1}, {"two", (void *)2}};
static void test_sections(void) {
    unsigned long sz = 0; const uint8_t *p = getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    CHECK(p == (const uint8_t *)t_ent && sz == sizeof t_ent, "getsectiondata finds a registry section (%lu bytes)", sz);
    CHECK(!getsectiondata(&_mh_execute_header, "__TEXT", "__dm_mainx0", &sz) && !sz, "an unknown section is not found");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = argc > 1 ? argv[1] : "/data/local/tmp";
    test_slots(); test_suspend(); test_memory(); test_context(); test_misc(); test_files(tmp); test_sections();
    printf("%d failed\n", g_fail);
    return g_fail;
}
