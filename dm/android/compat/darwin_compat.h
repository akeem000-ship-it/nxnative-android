// Darwin API subset for the Android host build: the host was written against Mach, libdispatch, QoS classes, malloc zones and CommonCrypto. The headers under
// dm/android/compat/include stand in for the Apple ones and all include this file; darwin_compat.c implements it on Linux / bionic. Only what the host uses is here, with the
// semantics it relies on (see each group). Built only for Android (dm/android/CMakeLists.txt puts include/ first on the include path)
#pragma once
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- TargetConditionals ----
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_MAC 0
#define TARGET_OS_SIMULATOR 0
#endif
#define DM_ANDROID 1
#ifndef ENOATTR
#define ENOATTR ENODATA   // Darwin's "no such attribute"
#endif

// ---- Mach basics: a "port" is a Linux thread ID (tid); the task is this process ----
typedef int kern_return_t;
typedef uint32_t mach_port_t, thread_act_t, mach_msg_type_number_t;
typedef mach_port_t *thread_act_array_t;
typedef uintptr_t vm_address_t, vm_offset_t, vm_size_t, mach_vm_address_t, mach_vm_size_t;
typedef int vm_prot_t, boolean_t;
typedef int *vm_region_info_t, *vm_region_recurse_info_t, *thread_info_t;
typedef uint64_t *thread_state_t;
enum { KERN_SUCCESS = 0, KERN_INVALID_ADDRESS = 1, KERN_PROTECTION_FAILURE = 2, KERN_NO_SPACE = 3, KERN_INVALID_ARGUMENT = 4, KERN_FAILURE = 5, KERN_RESOURCE_SHORTAGE = 6 };
#define MACH_PORT_NULL 0u
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
enum { VM_PROT_NONE = 0, VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4 };
enum { VM_FLAGS_FIXED = 0, VM_FLAGS_ANYWHERE = 1, VM_FLAGS_OVERWRITE = 0x4000 };
enum { VM_INHERIT_NONE = 2 };
extern vm_size_t vm_page_size;
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { return (mach_port_t)pthread_gettid_np(t); }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }

// Memory: vm_read_overwrite is a read that fails instead of faulting (process_vm_readv on this process); vm_allocate / vm_deallocate are anonymous mappings
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, int inherit);   // not supported (KERN_FAILURE): only the iOS embedding and a Mac probe use it
#define mach_vm_remap vm_remap
// Regions (from /proc/self/maps): the first mapping that contains *addr or lies above it; *addr and *size are set to it
typedef struct { vm_prot_t protection, max_protection; int inheritance; boolean_t shared, reserved; uint64_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, int flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *obj);
#define mach_vm_region vm_region_64

// Threads: task_threads lists /proc/self/task; thread_suspend stops a thread with a signal whose handler parks it after saving its registers, so thread_get_state reads them
// and thread_resume releases it. Diagnostics only (thread dumps, hitch sampling): the same rule as on Darwin holds between suspend and resume, no malloc / stdio / locks
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *count);
kern_return_t thread_suspend(thread_act_t t);
kern_return_t thread_resume(thread_act_t t);
// Same layout as Linux's sigcontext from regs[0] on (x0..x28, fp, lr, sp, pc, pstate): dm.h's DM_UC_SS reads a signal context through it
typedef struct __darwin_arm_thread_state64 { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define _STRUCT_ARM_THREAD_STATE64 struct __darwin_arm_thread_state64
struct ucontext;
__uint128_t *dm_uc_v(struct ucontext *uc);   // the FP / SIMD registers of a signal context (its FPSIMD record); NULL when it has none
uint32_t *dm_uc_fpsr(struct ucontext *uc), *dm_uc_fpcr(struct ucontext *uc);
uint64_t dm_uc_esr(struct ucontext *uc);       // the fault's syndrome (its ESR record), 0 when it has none
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(uint32_t)))
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t state, mach_msg_type_number_t *count);
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
enum { TH_STATE_RUNNING = 1, TH_STATE_STOPPED = 2, TH_STATE_WAITING = 3, TH_STATE_UNINTERRUPTIBLE = 4, TH_STATE_HALTED = 5 };
kern_return_t thread_info(thread_act_t t, int flavor, thread_info_t info, mach_msg_type_number_t *count);   // times from the thread's CPU clock (all counted as user time)

// task_info: TASK_VM_INFO (phys_footprint = resident, from /proc/self/status) and TASK_POWER_INFO_V2 (process CPU time; Android exposes no energy or wakeup counters to apps: 0)
typedef int *task_info_t;
typedef struct { uint64_t virtual_size, resident_size, resident_size_peak, phys_footprint; } task_vm_info_data_t;
typedef struct { struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } cpu_energy;
                 struct { uint64_t task_gpu_utilisation, task_gpu_stat_reserved0; } gpu_energy; uint64_t task_ptime, task_pset_switches, task_energy; } task_power_info_v2_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(int)))
kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count);
// Thread policies: an app cannot get real-time scheduling on Android, so the time-constraint policy is never in effect (set fails, get reports the default);
// THREAD_EXTENDED_INFO gives the nice value as the current and base priority
typedef int *thread_policy_t;
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
kern_return_t thread_policy_set(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n);
kern_return_t thread_policy_get(thread_act_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *get_default);
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
static inline int sysctlbyname(const char *name, void *old, size_t *oldn, void *nw, size_t nwn) { (void)name; (void)old; (void)oldn; (void)nw; (void)nwn; errno = ENOENT; return -1; }
// Task memory (memstat): resident and peak resident from /proc/self/status
typedef struct { uint64_t resident_size, resident_size_peak, phys_footprint; } dm_task_mem;
int dm_task_mem_read(dm_task_mem *m);

int thread_selfcounts(int type, void *buf, size_t nbytes);   // Darwin's per-thread {instructions, cycles}: zeros (Android gives apps no performance counters)

// ---- bionic thread data: where pthread key k's value lies in the calling thread's bionic_tls (slot -1 of the TLS slots tpidr_el0 points at: TLS_SLOT_BIONIC_TLS, which
// holds the key values since Android 10), the same offset in every thread; found by storing two different marks in the key and requiring exactly one word to hold each. ~0 when it is not found (dm/host/loader.c) ----
uint64_t dm_bionic_key_offset(pthread_key_t k);

// ---- Time: Mach absolute time is in nanoseconds here (timebase 1 / 1) ----
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline kern_return_t mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return KERN_SUCCESS; }
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
uint64_t mach_absolute_time(void);   // a function: the NVN profiling entry (nvn_entry.S) calls it
static inline uint64_t mach_continuous_time(void) { return clock_gettime_nsec_np(CLOCK_BOOTTIME); }
kern_return_t mach_wait_until(uint64_t deadline);   // absolute CLOCK_MONOTONIC nanoseconds
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
#endif
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull
#endif

// ---- pthread extensions ----
void *pthread_get_stackaddr_np(pthread_t t);   // the stack's top (highest address), as on Darwin
size_t pthread_get_stacksize_np(pthread_t t);
int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel);
#define pthread_setname_np(name) pthread_setname_np(pthread_self(), (name))   // Darwin names only the calling thread; bionic truncates past 15 bytes itself

// QoS classes become nice values of the thread (setpriority on its tid): interactive -8, initiated -4, default 0, utility 4, background 10. Overrides are not supported
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09,
               QOS_CLASS_UNSPECIFIED = 0x00 } qos_class_t;
typedef void *pthread_override_t;
int pthread_set_qos_class_self_np(qos_class_t q, int rel);
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel);
int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel);   // recorded nowhere: the thread sets its own class when it starts
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return (pthread_override_t)1; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }
qos_class_t qos_class_self(void);

// ---- libdispatch: semaphores only (POSIX semaphores); timeouts are relative nanoseconds from dispatch_time(DISPATCH_TIME_NOW, ns) ----
typedef sem_t *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);   // absolute CLOCK_REALTIME nanoseconds (what sem_timedwait takes)
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t t);   // 0 when signalled, non-zero on timeout
void dispatch_release(dispatch_semaphore_t s);

// ---- malloc zones: one heap; a zone is a tag ----
typedef struct malloc_zone { const char *name; } malloc_zone_t;
malloc_zone_t *malloc_create_zone(vm_size_t start, unsigned flags);
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { z->name = n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_calloc(malloc_zone_t *z, size_t c, size_t n) { (void)z; return calloc(c, n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; return memalign(a < sizeof(void *) ? sizeof(void *) : a, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
static inline size_t malloc_size(const void *p) { return p ? malloc_usable_size(p) : 0; }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
static inline malloc_zone_t *malloc_default_zone(void) { static malloc_zone_t z = { "default" }; return &z; }

// ---- Caches ----
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }

// ---- File descriptors ----
#ifndef F_GETPATH
#define F_GETPATH 50   // fcntl(fd, F_GETPATH, buf) → dm_fd_path (the fcntl wrapper below)
#endif
int dm_fd_path(int fd, char *buf);   // readlink of /proc/self/fd/<fd> into a PATH_MAX buffer; 0 / -1 as fcntl
#define fcntl(fd, cmd, ...) ((cmd) == F_GETPATH ? dm_fd_path((fd), (char *)(__VA_ARGS__)) : (fcntl)((fd), (cmd), ##__VA_ARGS__))
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }

// ---- CommonCrypto: SHA-256 ----
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);

// ---- Mach-O sections (getsectiondata): the host's registry sections (dm.h DM_SECTION: swhle, swforce, swnvn in __DATA) are ELF sections here, found through the
// linker's __start_ / __stop_ symbols; anything else (the iOS embedded image) finds nothing ----
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size);

#ifdef __cplusplus
}
#endif
