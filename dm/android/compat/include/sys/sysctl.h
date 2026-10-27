// Android stand-in for Darwin's <sys/sysctl.h>: only the process-start query main.c makes (KERN_PROC_PID), which fails here; main.c then counts the boot timeline from host start
#pragma once
#include <errno.h>
#include <stddef.h>
#include <sys/time.h>
enum { CTL_KERN = 1, KERN_PROC = 14, KERN_PROC_PID = 1 };
struct kinfo_proc { struct { struct timeval p_starttime; } kp_proc; };
static inline int sysctl(int *mib, unsigned n, void *old, size_t *oldn, void *nw, size_t nwn) { (void)mib; (void)n; (void)old; (void)oldn; (void)nw; (void)nwn; errno = ENOTSUP; return -1; }
