// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void);
#if defined(__ANDROID__)
// No libdispatch on Android: the 1.5 s fallback is a thread that sleeps, and SIGTERM reaches an ordinary thread through a pipe (the handler only writes one byte)
static void *exit_fallback(void *a) {
    (void)a; struct timespec ts = {1, 500000000}; while (nanosleep(&ts, &ts) && errno == EINTR) {}
    LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0);
}
static int g_term_pipe[2] = {-1, -1};
static void on_term(int sig) { (void)sig; char c = 1; ssize_t r = write(g_term_pipe[1], &c, 1); (void)r; }
static void *term_watch(void *a) {
    (void)a; char c;
    while (read(g_term_pipe[0], &c, 1) < 0 && errno == EINTR) {}
    dm_host_shutdown("SIGTERM"); return NULL;
}
#endif
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
#if defined(__ANDROID__)
    pthread_t fb; if (!pthread_create(&fb, NULL, exit_fallback, NULL)) pthread_detach(fb);
#else
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0); });
#endif
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
#if defined(__ANDROID__)
    pthread_t th;
    if (pipe(g_term_pipe) || pthread_create(&th, NULL, term_watch, NULL)) { LOG("[RED] SIGTERM watcher not started: the default action exits"); return; }
    pthread_detach(th); signal(SIGTERM, on_term);
    return;
#endif
#if !defined(__ANDROID__)
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#endif
}
