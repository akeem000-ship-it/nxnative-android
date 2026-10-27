// Linux / bionic implementation of the Darwin subset in darwin_compat.h (see there for what each group does)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "darwin_compat.h"
#include <dirent.h>
#include <stdio.h>
#include "include/sys/clonefile.h"
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <ucontext.h>

vm_size_t vm_page_size = 4096;
const struct mach_header_64 _mh_execute_header = {0};
__attribute__((constructor)) static void page_size_init(void) { vm_page_size = (vm_size_t)sysconf(_SC_PAGESIZE); }

// ---- Registry sections ----
#define SECTION_BOUNDS(n) extern const uint8_t __start_##n[] __attribute__((weak, visibility("hidden"))), __stop_##n[] __attribute__((weak, visibility("hidden")));
SECTION_BOUNDS(swhle) SECTION_BOUNDS(swforce) SECTION_BOUNDS(swnvn)
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; const uint8_t *a = NULL, *b = NULL;
    if (!strcmp(seg, "__DATA")) {
        if (!strcmp(sect, "swhle")) { a = __start_swhle; b = __stop_swhle; }
        else if (!strcmp(sect, "swforce")) { a = __start_swforce; b = __stop_swforce; }
        else if (!strcmp(sect, "swnvn")) { a = __start_swnvn; b = __stop_swnvn; }
    }
    if (size) *size = a && b > a ? (unsigned long)(b - a) : 0;
    return a && b > a ? (uint8_t *)a : NULL;
}

// ---- Memory ----
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n < 0) {
        // process_vm_readv can be refused (seccomp, ptrace scope): fall back to a pipe, which the kernel fills with EFAULT checks instead of faulting
        static __thread int pfd[2] = {-1, -1};
        if (pfd[0] < 0 && pipe2(pfd, O_CLOEXEC | O_NONBLOCK)) { if (out) *out = 0; return KERN_FAILURE; }
        size_t done = 0;
        while (done < size) {
            size_t k = size - done < 4096 ? size - done : 4096;
            ssize_t w = write(pfd[1], (const void *)(addr + done), k);
            if (w <= 0) break;
            ssize_t rd = read(pfd[0], (void *)(data + done), (size_t)w);
            if (rd != w) break;
            done += (size_t)w;
        }
        if (out) *out = done;
        return done == size ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
    }
    if (out) *out = (vm_size_t)n;
    return (vm_size_t)n == size ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *hint = (flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr;
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (hint ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_INVALID_ADDRESS : KERN_SUCCESS; }
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, int inherit) {
    (void)task; (void)to; (void)size; (void)mask; (void)flags; (void)src_task; (void)from; (void)copy; (void)inherit;
    if (cur) *cur = 0; if (max) *max = 0;
    return KERN_FAILURE;
}
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, int flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj) {
    (void)task; (void)flavor;
    if (obj) *obj = MACH_PORT_NULL;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return KERN_FAILURE;
    // Read in blocks and parse line by line without stdio (callers may be in a crash or a fault handler)
    char buf[8192], line[512]; size_t ln = 0; ssize_t n; kern_return_t kr = KERN_INVALID_ADDRESS;
    while (kr != KERN_SUCCESS && (n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n && kr != KERN_SUCCESS; i++) {
            if (buf[i] != '\n') { if (ln < sizeof line - 1) line[ln++] = buf[i]; continue; }
            line[ln] = 0; ln = 0;
            unsigned long lo, hi; char pr[5];
            if (sscanf(line, "%lx-%lx %4s", &lo, &hi, pr) != 3 || hi <= *addr) continue;
            vm_region_basic_info_data_64_t *b = (vm_region_basic_info_data_64_t *)info;
            memset(b, 0, sizeof *b);
            b->protection = (pr[0] == 'r' ? VM_PROT_READ : 0) | (pr[1] == 'w' ? VM_PROT_WRITE : 0) | (pr[2] == 'x' ? VM_PROT_EXECUTE : 0);
            b->max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE; b->shared = pr[3] == 's';
            *addr = lo; *size = hi - lo; if (count) *count = VM_REGION_BASIC_INFO_COUNT_64;
            kr = KERN_SUCCESS;
        }
    }
    close(fd);
    return kr;
}

// ---- Threads ----
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *count) {
    (void)task;
    DIR *d = opendir("/proc/self/task");
    if (!d) return KERN_FAILURE;
    size_t cap = 256, n = 0; vm_address_t a;
    if (vm_allocate(0, &a, cap * sizeof(mach_port_t), VM_FLAGS_ANYWHERE)) { closedir(d); return KERN_RESOURCE_SHORTAGE; }
    mach_port_t *t = (mach_port_t *)a;
    for (struct dirent *e; (e = readdir(d)) && n < cap;) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') t[n++] = (mach_port_t)atoi(e->d_name);
    closedir(d);
    *list = t; *count = (mach_msg_type_number_t)n;
    return KERN_SUCCESS;
}
// Suspension: one thread at a time (the callers suspend, read and resume in turn, or a list in order: up to 64 parked at once)
#define SUSP_MAX 64
static struct { _Atomic int tid; _Atomic uint32_t state; arm_thread_state64_t st; } g_susp[SUSP_MAX];   // state: 0 free, 1 asked, 2 parked, 3 released
static int g_susp_sig;
static long futex(_Atomic uint32_t *a, int op, uint32_t v, const struct timespec *t) { return syscall(SYS_futex, a, op, v, t, NULL, 0); }
static void on_suspend(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; ucontext_t *uc = uc_; int me = gettid(), e = errno;
    for (int i = 0; i < SUSP_MAX; i++) if (atomic_load(&g_susp[i].tid) == me && atomic_load(&g_susp[i].state) == 1) {
        memcpy(&g_susp[i].st, &uc->uc_mcontext.regs[0], sizeof g_susp[i].st);
        atomic_store(&g_susp[i].state, 2); futex(&g_susp[i].state, FUTEX_WAKE_PRIVATE, 1, NULL);
        while (atomic_load(&g_susp[i].state) == 2) futex(&g_susp[i].state, FUTEX_WAIT_PRIVATE, 2, NULL);
        atomic_store(&g_susp[i].tid, 0); atomic_store(&g_susp[i].state, 0);
        break;
    }
    errno = e;
}
__attribute__((constructor)) static void susp_init(void) {
    g_susp_sig = SIGRTMIN + 3;
    struct sigaction sa = {0}; sa.sa_sigaction = on_suspend; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigfillset(&sa.sa_mask);
    sigaction(g_susp_sig, &sa, NULL);
}
kern_return_t thread_suspend(thread_act_t t) {
    if ((int)t == gettid()) return KERN_INVALID_ARGUMENT;
    int i = 0, z = 0;
    for (; i < SUSP_MAX; i++) { z = 0; if (atomic_compare_exchange_strong(&g_susp[i].tid, &z, (int)t)) break; }
    if (i == SUSP_MAX) return KERN_RESOURCE_SHORTAGE;
    atomic_store(&g_susp[i].state, 1);
    if (syscall(SYS_tgkill, getpid(), (int)t, g_susp_sig)) { atomic_store(&g_susp[i].state, 0); atomic_store(&g_susp[i].tid, 0); return KERN_INVALID_ARGUMENT; }
    // Wait up to 200 ms for the thread to park (a thread blocked with the signal masked, or exiting, never does)
    struct timespec ts = {0, 20000000};
    for (int k = 0; k < 10 && atomic_load(&g_susp[i].state) == 1; k++) futex(&g_susp[i].state, FUTEX_WAIT_PRIVATE, 1, &ts);
    uint32_t one = 1;
    if (atomic_compare_exchange_strong(&g_susp[i].state, &one, 0)) { atomic_store(&g_susp[i].tid, 0); return KERN_FAILURE; }   // never parked: give the slot back (a late signal finds no request)
    return KERN_SUCCESS;
}
static int susp_slot(thread_act_t t) { for (int i = 0; i < SUSP_MAX; i++) if (atomic_load(&g_susp[i].tid) == (int)t && atomic_load(&g_susp[i].state) == 2) return i; return -1; }
kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t state, mach_msg_type_number_t *count) {
    int i = susp_slot(t);
    if (flavor != ARM_THREAD_STATE64 || i < 0) return KERN_FAILURE;   // like Darwin for a running thread's live state: only a suspended one is read here
    memcpy(state, &g_susp[i].st, sizeof g_susp[i].st); if (count) *count = ARM_THREAD_STATE64_COUNT;
    return KERN_SUCCESS;
}
kern_return_t thread_resume(thread_act_t t) {
    int i = susp_slot(t);
    if (i < 0) return KERN_FAILURE;
    atomic_store(&g_susp[i].state, 3); futex(&g_susp[i].state, FUTEX_WAKE_PRIVATE, 1, NULL);
    return KERN_SUCCESS;
}
kern_return_t thread_policy_set(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)flavor; (void)p; (void)n; return KERN_FAILURE; }
kern_return_t thread_policy_get(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *get_default) {
    (void)t; (void)flavor; if (p && n && *n >= THREAD_TIME_CONSTRAINT_POLICY_COUNT) memset(p, 0, sizeof(thread_time_constraint_policy_data_t)); if (get_default) *get_default = TRUE; return KERN_SUCCESS;
}
kern_return_t thread_info(thread_act_t t, int flavor, thread_info_t info, mach_msg_type_number_t *count) {
    if (flavor == THREAD_EXTENDED_INFO) {
        thread_extended_info_data_t *x = (thread_extended_info_data_t *)info; memset(x, 0, sizeof *x);
        errno = 0; int nice = getpriority(PRIO_PROCESS, (id_t)t); if (nice == -1 && errno) return KERN_INVALID_ARGUMENT;
        x->pth_curpri = x->pth_priority = -nice; if (count) *count = THREAD_EXTENDED_INFO_COUNT;
        return KERN_SUCCESS;
    }
    if (flavor != THREAD_BASIC_INFO) return KERN_INVALID_ARGUMENT;
    thread_basic_info_data_t *b = (thread_basic_info_data_t *)info; memset(b, 0, sizeof *b);
    struct timespec ts; clockid_t c = (clockid_t)((~(unsigned)t << 3) | 6);   // the thread's CPU clock (CPUCLOCK_SCHED | CPUCLOCK_PERTHREAD_MASK)
    if (clock_gettime(c, &ts)) return KERN_INVALID_ARGUMENT;
    b->user_time.seconds = (int)ts.tv_sec; b->user_time.microseconds = (int)(ts.tv_nsec / 1000);
    char p[64], s[512]; snprintf(p, sizeof p, "/proc/self/task/%u/stat", t);
    int fd = open(p, O_RDONLY | O_CLOEXEC); b->run_state = TH_STATE_WAITING;
    if (fd >= 0) { ssize_t n = read(fd, s, sizeof s - 1); close(fd);
        if (n > 0) { s[n] = 0; char *q = strrchr(s, ')'); if (q && q[1] == ' ') b->run_state = q[2] == 'R' ? TH_STATE_RUNNING : q[2] == 'T' || q[2] == 't' ? TH_STATE_STOPPED : q[2] == 'D' ? TH_STATE_UNINTERRUPTIBLE : TH_STATE_WAITING; } }
    if (count) *count = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}
int dm_task_mem_read(dm_task_mem *m) {
    memset(m, 0, sizeof *m);
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    char s[4096]; ssize_t n = read(fd, s, sizeof s - 1); close(fd); if (n <= 0) return -1; s[n] = 0;
    char *p;
    if ((p = strstr(s, "VmRSS:"))) m->resident_size = strtoull(p + 6, NULL, 10) * 1024;
    if ((p = strstr(s, "VmHWM:"))) m->resident_size_peak = strtoull(p + 6, NULL, 10) * 1024;
    m->phys_footprint = m->resident_size;
    return 0;
}

kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task;
    if (flavor == TASK_VM_INFO) {
        dm_task_mem m; if (dm_task_mem_read(&m)) return KERN_FAILURE;
        task_vm_info_data_t *v = (task_vm_info_data_t *)info; memset(v, 0, sizeof *v);
        v->resident_size = m.resident_size; v->resident_size_peak = m.resident_size_peak; v->phys_footprint = m.phys_footprint;
        if (count) *count = TASK_VM_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == TASK_POWER_INFO_V2) {
        task_power_info_v2_data_t *p = (task_power_info_v2_data_t *)info; memset(p, 0, sizeof *p);
        p->cpu_energy.total_user = clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID);   // nanoseconds (the timebase is 1 / 1)
        if (count) *count = TASK_POWER_INFO_V2_COUNT; return KERN_SUCCESS;
    }
    return KERN_INVALID_ARGUMENT;
}

// ---- bionic thread data ----
uint64_t dm_bionic_key_offset(pthread_key_t k) {
    static const uint64_t mark[2] = {0x6e786e61746976e1ull, 0x9187919e8b968901ull};
    uint64_t *tls; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
    const uint64_t b = tls[-1]; uint64_t at = ~0ull;
    void *was = pthread_getspecific(k);
    // The key data sits within the first 16 KB of bionic_tls, but what follows it may be unmapped: copy what is readable with fault-safe reads, a page at a time, and scan the copy
    uint64_t buf[0x4000 / 8]; size_t len = 0;
    while (len < sizeof buf) {   // a page at a time: a read within one page succeeds or fails whole
        size_t want = len ? 4096 : 4096 - (size_t)(b & 4095); if (len + want > sizeof buf) want = sizeof buf - len;
        vm_size_t got = 0; vm_read_overwrite(0, (vm_address_t)(b + len), want, (vm_address_t)((uint8_t *)buf + len), &got);
        len += got; if (got < want) break;
    }
    for (int r = 0; r < 2; r++) {
        if (pthread_setspecific(k, (void *)mark[r])) return ~0ull;
        uint64_t hit = ~0ull; int n = 0;
        vm_size_t got = 0; vm_read_overwrite(0, (vm_address_t)b, len, (vm_address_t)buf, &got);
        for (uint64_t i = 0; i < got / 8; i++) if (buf[i] == mark[r]) { hit = i * 8; n++; }
        if (n != 1 || (r && hit != at)) { pthread_setspecific(k, was); return ~0ull; }
        at = hit;
    }
    pthread_setspecific(k, was);
    return at;
}

// ---- Time ----
kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return KERN_SUCCESS;
}
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

// ---- pthread ----
void *pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a; void *lo = NULL; size_t sz = 0;
    if (pthread_getattr_np(t, &a)) return NULL;
    pthread_attr_getstack(&a, &lo, &sz); pthread_attr_destroy(&a);
    return (char *)lo + sz;
}
size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; void *lo = NULL; size_t sz = 0;
    if (pthread_getattr_np(t, &a)) return 0;
    pthread_attr_getstack(&a, &lo, &sz); pthread_attr_destroy(&a);
    return sz;
}
int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += rel->tv_sec; ts.tv_nsec += rel->tv_nsec; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait_monotonic_np(c, m, &ts);
}
static __thread qos_class_t t_qos = QOS_CLASS_DEFAULT;
static int qos_nice(qos_class_t q) {
    switch (q) { case QOS_CLASS_USER_INTERACTIVE: return -8; case QOS_CLASS_USER_INITIATED: return -4; case QOS_CLASS_UTILITY: return 4; case QOS_CLASS_BACKGROUND: return 10; default: return 0; }
}
int pthread_set_qos_class_self_np(qos_class_t q, int rel) {
    (void)rel; t_qos = q;
    // Raising a thread above the default needs no permission on Android for an app's own threads down to -10 (it is how the framework's audio and render threads run)
    return setpriority(PRIO_PROCESS, (id_t)gettid(), qos_nice(q)) ? errno : 0;
}
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { if (q) *q = pthread_equal(t, pthread_self()) ? t_qos : QOS_CLASS_UNSPECIFIED; if (rel) *rel = 0; return 0; }
int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel) { (void)a; (void)q; (void)rel; return 0; }
qos_class_t qos_class_self(void) { return t_qos; }

// ---- dispatch semaphores ----
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (uint64_t)(delta > 0 ? delta : 0);
}
dispatch_semaphore_t dispatch_semaphore_create(long value) { sem_t *s = malloc(sizeof *s); if (s && sem_init(s, 0, (unsigned)value)) { free(s); s = NULL; } return s; }
long dispatch_semaphore_signal(dispatch_semaphore_t s) { return sem_post(s); }
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t t) {
    if (t == DISPATCH_TIME_FOREVER) { while (sem_wait(s) && errno == EINTR) {} return 0; }
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    int r; while ((r = sem_timedwait(s, &ts)) && errno == EINTR) {}
    return r ? 1 : 0;
}
void dispatch_release(dispatch_semaphore_t s) { if (s) { sem_destroy(s); free(s); } }

// ---- malloc zones ----
malloc_zone_t *malloc_create_zone(vm_size_t start, unsigned flags) { (void)start; (void)flags; malloc_zone_t *z = calloc(1, sizeof *z); z->name = "zone"; return z; }

// ---- File descriptors, clonefile, xattr ----
int dm_fd_path(int fd, char *buf) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, buf, PATH_MAX - 1);
    if (n < 0) return -1;
    buf[n] = 0; return 0;
}
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[1 << 16]; ssize_t n; int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) for (ssize_t w = 0; w < n;) { ssize_t k = write(out, buf + w, (size_t)(n - w)); if (k < 0) { r = -1; break; } w += k; }
    if (n < 0) r = -1;
    int e = errno; if (fsync(out)) r = -1; close(in); if (close(out)) r = -1; errno = e;
    return r;
}
static void copy_xattrs(const char *src, const char *dst) {
    char names[4096]; ssize_t n = llistxattr(src, names, sizeof names);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[1024]; ssize_t k = lgetxattr(src, names + i, v, sizeof v);
        if (k >= 0) lsetxattr(dst, names + i, v, (size_t)k, 0);
    }
}
int clonefile(const char *src, const char *dst, uint32_t flags) {
    (void)flags;
    struct stat st; if (lstat(src, &st)) return -1;
    if (!access(dst, F_OK) || errno != ENOENT) { errno = EEXIST; return -1; }
    if (!S_ISDIR(st.st_mode)) { if (copy_file(src, dst, st.st_mode)) return -1; copy_xattrs(src, dst); return 0; }
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    copy_xattrs(src, dst);
    DIR *d = opendir(src); if (!d) return -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        r = clonefile(a, b, flags);
    }
    int er = errno; closedir(d); errno = er;
    return r;
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags & RENAME_SWAP) return renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCHANGE);
    if (flags & RENAME_EXCL) return renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
    return rename(from, to);
}
// Darwin attribute names have no namespace; Linux lets an app write only user.*
static const char *xname(const char *n, char *buf, size_t sz) { snprintf(buf, sz, "user.%s", n); return buf; }
#undef getxattr
#undef setxattr
#undef removexattr
ssize_t dm_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; return (opt & XATTR_NOFOLLOW) ? lgetxattr(p, xname(n, b, sizeof b), v, s) : getxattr(p, xname(n, b, sizeof b), v, s);
}
int dm_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; return (opt & XATTR_NOFOLLOW) ? lsetxattr(p, xname(n, b, sizeof b), v, s, 0) : setxattr(p, xname(n, b, sizeof b), v, s, 0);
}
int dm_removexattr(const char *p, const char *n, int opt) {
    char b[256]; return (opt & XATTR_NOFOLLOW) ? lremovexattr(p, xname(n, b, sizeof b)) : removexattr(p, xname(n, b, sizeof b));
}

// ---- Signal context records (FPSIMD, ESR) ----
static void *uc_record(struct ucontext *uc_, uint32_t magic) {
    ucontext_t *uc = (ucontext_t *)uc_;
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        uint32_t m = ((uint32_t *)p)[0], size = ((uint32_t *)p)[1];
        if (!m || size < 8) break;
        if (m == magic) return p;
        p += size;
    }
    return NULL;
}
__uint128_t *dm_uc_v(struct ucontext *uc) { struct fpsimd_context *f = uc_record(uc, FPSIMD_MAGIC); return f ? (__uint128_t *)f->vregs : NULL; }
static uint32_t g_no_fp;
uint32_t *dm_uc_fpsr(struct ucontext *uc) { struct fpsimd_context *f = uc_record(uc, FPSIMD_MAGIC); return f ? &f->fpsr : &g_no_fp; }
uint32_t *dm_uc_fpcr(struct ucontext *uc) { struct fpsimd_context *f = uc_record(uc, FPSIMD_MAGIC); return f ? &f->fpcr : &g_no_fp; }
uint64_t dm_uc_esr(struct ucontext *uc) { struct esr_context *e = uc_record(uc, ESR_MAGIC); return e ? e->esr : 0; }

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i], t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->n = 0; c->used = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data; c->n += len;
    if (c->used) { uint32_t k = 64 - c->used < len ? 64 - c->used : len; memcpy(c->buf + c->used, p, k); c->used += k; p += k; len -= k; if (c->used == 64) { sha_block(c->h, c->buf); c->used = 0; } }
    for (; len >= 64; p += 64, len -= 64) sha_block(c->h, p);
    if (len) { memcpy(c->buf, p, len); c->used = len; }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1); while (c->used != 56) CC_SHA256_Update(c, &z, 1);
    uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, L, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, data, len); CC_SHA256_Final(md, &c); return md; }

// ---- CoreFoundation: pointer-keyed dictionary (open addressing, tombstones on removal) ----
#include "include/CoreFoundation/CoreFoundation.h"
struct dm_cfdict { const void **k, **v; size_t cap, n, used; };
static const void *const TOMB = (const void *)&TOMB;
static size_t cf_hash(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (size_t)x & (cap - 1); }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *keycb, const void *valcb) {
    (void)a; (void)cap;
    if (keycb || valcb) { fprintf(stderr, "[dm] FATAL CFDictionaryCreateMutable: only NULL callbacks are supported on Android\n"); abort(); }
    struct dm_cfdict *d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v);
    return d;
}
static size_t cf_find(const struct dm_cfdict *d, const void *key) {   // the key's slot, or cap when absent
    for (size_t i = cf_hash(key, d->cap), n = 0; n < d->cap; i = (i + 1) & (d->cap - 1), n++) { if (!d->k[i]) return d->cap; if (d->k[i] == key) return i; }
    return d->cap;
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key) { size_t i = cf_find(d, key); return i < d->cap ? d->v[i] : NULL; }
static void cf_grow(struct dm_cfdict *d) {
    const void **k = d->k, **v = d->v; size_t cap = d->cap;
    d->cap = d->n * 4 > cap ? cap * 2 : cap; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->n = d->used = 0;
    for (size_t i = 0; i < cap; i++) if (k[i] && k[i] != TOMB) CFDictionarySetValue(d, k[i], v[i]);
    free(k); free(v);
}
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value) {
    size_t i = cf_find(d, key); if (i < d->cap) { d->v[i] = value; return; }
    if ((d->used + 1) * 4 > d->cap * 3) cf_grow(d);
    for (i = cf_hash(key, d->cap); d->k[i] && d->k[i] != TOMB; i = (i + 1) & (d->cap - 1)) {}
    if (!d->k[i]) d->used++;
    d->k[i] = key; d->v[i] = value; d->n++;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key) { size_t i = cf_find(d, key); if (i < d->cap) { d->k[i] = TOMB; d->v[i] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(CFTypeRef p) { struct dm_cfdict *d = (struct dm_cfdict *)p; if (!d) return; free(d->k); free(d->v); free(d); }
