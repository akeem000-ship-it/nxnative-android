// Android front end (NativeActivity): the same-named interface as the Mac front end (dm/host/front_mac.m) and the iOS app (dm/ios), front.h.
//   · Boot (as dm/ios/Sources/main.m): the host log goes to <external files>/log.txt and to logcat; env.txt there sets environment switches (KEY=VALUE per line, # comments);
//     the rewritten modules and tables packed into the APK (assets/dm, listed in assets/dm/files.txt) are copied to internal storage once per APK build; the game's RomFS is
//     <external files>/romfs (pushed by dm/android/tools/build_apk.py --push or adb); saves and temporary files are in internal storage. Then the host's main runs on a 64 MB stack thread.
//   · Lifecycle (as front_ios.m): focus lost / gained → dm_focus_set 2 / 1; the app stopped (background) → focus 3, audio paused, guest clock frozen, graphics suspended; started again → the reverse.
//   · Input: game controllers by position (right A, bottom B, top X, left Y, as front_mac.m and DmGamepad.m), the left / right sticks, the d-pad as keys or as a hat, the triggers as ZL / ZR;
//     up to DM_PLAYERS controllers, the first on player 1. Touches go to the touchscreen (dm_touch_set), normalized to the window.
//   · front_metal_layer gives the ANativeWindow (the graphics backend's surface); there is no progress screen or text input box yet (logged; text input reports no window)
#include "../host/dm.h"
#include "../host/dm_input.h"
#include "../host/front.h"
#include "../host/hle.h"
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "nxnative"
void guest_clock_freeze_for(int reason, int on);
int main(int argc, char **argv);   // dm/host/main.c

static struct android_app *g_app;
static _Atomic(ANativeWindow *) g_window;
static char g_ext[512], g_int[512];

// ---- Log: stderr → log.txt and logcat ----
static int g_logfd = -1;
static void *log_pump(void *p) {
    int fd = (int)(intptr_t)p; char buf[4096], line[1024]; size_t ln = 0;
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) {
        if (g_logfd >= 0) { ssize_t w = write(g_logfd, buf, (size_t)n); (void)w; }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n' && ln < sizeof line - 1) { line[ln++] = buf[i]; continue; }
            line[ln] = 0; ln = 0; __android_log_write(strstr(line, "[RED]") || strstr(line, "FATAL") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, TAG, line);
        }
    }
    return NULL;
}
static void log_start(void) {
    char p[600]; snprintf(p, sizeof p, "%s/log.txt", g_ext);
    g_logfd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int fds[2]; if (pipe(fds)) return;
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(fds[1], STDERR_FILENO); close(fds[1]);
    pthread_t t; pthread_create(&t, NULL, log_pump, (void *)(intptr_t)fds[0]); pthread_detach(t);
}

// ---- Environment files ----
static void env_file(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || !*s || *s == '\n') continue;
        char *e = strchr(s, '='); if (!e) continue; *e = 0;
        char *v = e + 1; size_t n = strlen(v); while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
        if (!getenv(s)) { setenv(s, v, 0); fprintf(stderr, "boot: env %s=%s (%s)\n", s, v, path); }
    }
    fclose(f);
}

// ---- Packed files (assets/dm) → internal storage ----
static int mkdirs(const char *path) {
    char p[1024]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
static char *asset_text(AAssetManager *am, const char *name) {
    AAsset *a = AAssetManager_open(am, name, AASSET_MODE_BUFFER); if (!a) return NULL;
    off_t n = AAsset_getLength(a); char *s = malloc((size_t)n + 1);
    memcpy(s, AAsset_getBuffer(a), (size_t)n); s[n] = 0; AAsset_close(a); return s;
}
static void unpack_dm(AAssetManager *am, const char *dst) {
    // files.txt: the APK build's stamp on the first line, then one relative path per line
    char *list = asset_text(am, "dm/files.txt");
    if (!list) die("the APK has no assets/dm/files.txt: it was not packed by dm/android/tools/build_apk.py");
    char stampf[600], *stamp_old = NULL; snprintf(stampf, sizeof stampf, "%s/.stamp", dst);
    FILE *sf = fopen(stampf, "r"); if (sf) { char b[256] = ""; if (fgets(b, sizeof b, sf)) stamp_old = strdup(b); fclose(sf); }
    char *nl = strchr(list, '\n'); if (!nl) die("assets/dm/files.txt is empty");
    *nl = 0; const char *stamp = list;
    if (stamp_old && !strncmp(stamp_old, stamp, strlen(stamp))) { LOG("packed game files already in place (%s)", stamp); free(list); free(stamp_old); return; }
    uint64_t t0 = now_ns(); unsigned files = 0; uint64_t bytes = 0;
    unlink(stampf); mkdirs(dst);
    for (char *p = nl + 1, *e; *p; p = e + 1) {
        e = strchr(p, '\n'); if (!e) e = p + strlen(p) - 1; else *e = 0;
        if (!*p) continue;
        char an[1024], out[1024]; snprintf(an, sizeof an, "dm/%s", p); snprintf(out, sizeof out, "%s/%s", dst, p);
        char dir[1024]; snprintf(dir, sizeof dir, "%s", out); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdirs(dir); }
        AAsset *a = AAssetManager_open(am, an, AASSET_MODE_STREAMING); if (!a) die("packed file %s missing from the APK", an);
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) die("cannot write %s", out);
        char buf[1 << 16]; for (int n; (n = AAsset_read(a, buf, sizeof buf)) > 0;) { if (write(fd, buf, (size_t)n) != n) die("writing %s failed (storage full?)", out); bytes += (uint64_t)n; }
        close(fd); AAsset_close(a); files++;
        if (!*e) break;
    }
    sf = fopen(stampf, "w"); if (sf) { fprintf(sf, "%s\n", stamp); fclose(sf); }
    LOG("unpacked %u game files (%.1f MB) in %.1f s (%s)", files, bytes / 1048576.0, (now_ns() - t0) / 1e9, stamp);
    free(list); free(stamp_old);
}

// ---- Boot ----
static int romfs_ok(const char *dir) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, DM_GAME_ROMFS_MARKER);
    struct stat st; return !stat(p, &st);
}
static _Atomic int g_host_started;
static char *g_argv[3];
static void *host_thread(void *a) {
    (void)a;
    int rc = main(2, g_argv);
    LOG("boot: the host's main returned %d", rc);
    ANativeActivity_finish(g_app->activity);
    return NULL;
}
static void boot(void) {
    ANativeActivity *act = g_app->activity;
    snprintf(g_int, sizeof g_int, "%s", act->internalDataPath);
    snprintf(g_ext, sizeof g_ext, "%s", act->externalDataPath ? act->externalDataPath : act->internalDataPath);
    mkdirs(g_int); mkdirs(g_ext);
    log_start();
    fprintf(stderr, "boot: nxnative for Android, %s (%s); files in %s, internal %s\n", DM_GAME_NAME, DM_GAME_ID, g_ext, g_int);
    char p[1024];
    snprintf(p, sizeof p, "%s/env.txt", g_ext); env_file(p);
    char lang[3] = "", country[3] = ""; AConfiguration_getLanguage(g_app->config, lang); AConfiguration_getCountry(g_app->config, country);
    if (!getenv("DM_LANG") && lang[0]) { char l[16]; snprintf(l, sizeof l, "%c%c-%c%c", lang[0], lang[1], country[0] ? country[0] : 'U', country[0] ? country[1] : 'S'); setenv("DM_LANG", l, 0); }
    fprintf(stderr, "boot: game language %s\n", getenv("DM_LANG") ? getenv("DM_LANG") : "en-US");
    char dm[600]; snprintf(dm, sizeof dm, "%s/dm", g_int);
    unpack_dm(act->assetManager, dm);
    char romfs[600]; snprintf(romfs, sizeof romfs, "%s/romfs", g_ext);
    if (!romfs_ok(romfs)) {
        LOG("[RED] no game data: %s/%s is missing. Copy the game's RomFS there (dm/android/tools/build_apk.py --push, or adb push <romfs> %s) and start the app again",
            romfs, DM_GAME_ROMFS_MARKER, romfs);
        return;
    }
    setenv("DM_ROMFS", romfs, 0);
    snprintf(p, sizeof p, "%s/save", g_int); mkdirs(p); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", g_int); mkdirs(p); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/shader_map.tsv", dm); setenv("DM_NVN_SHMAP", p, 0);
    snprintf(p, sizeof p, "%s/font", dm); setenv("DM_FONT_DIR", p, 0);
    snprintf(p, sizeof p, "%s/aoc", g_ext); struct stat st; if (!stat(p, &st)) setenv("DM_AOC_DIR", p, 0);
    if (chdir(g_int)) LOG("[RED] cannot enter %s", g_int);
    g_argv[0] = strdup("dmhost"); g_argv[1] = strdup(dm);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);   // as the iOS app: the Switch main thread's stack is set by npdm; the host gives plenty
    pthread_t t; if (pthread_create(&t, &at, host_thread, NULL)) die("cannot start the host thread");
    pthread_detach(t); atomic_store(&g_host_started, 1);
}

// ---- front.h ----
void front_run(void (*guest)(void)) { guest(); }   // already on the host's big-stack thread
void *front_metal_layer(void) { return atomic_load(&g_window); }
int front_screen_max_fps(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) {
    static long last = -1; if (!title) { last = -1; return; }
    if (total > 0 && done * 10 / total != last) { last = done * 10 / total; LOG("%s: %ld / %ld", title, done, total); }
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    LOG("[RED] text input \"%s\": the Android front end has no input box yet, reported as no window", title ? title : "");
    return -1;
}

// ---- Input ----
#define NPAD DM_PLAYERS
static struct { int32_t dev; uint64_t keys, hat; int32_t lx, ly, rx, ry; float lt, rt; } g_pad[NPAD];
static int g_npad;
static int pad_of(int32_t dev) {
    for (int i = 0; i < g_npad; i++) if (g_pad[i].dev == dev) return i;
    if (g_npad == NPAD) return -1;
    memset(&g_pad[g_npad], 0, sizeof g_pad[0]); g_pad[g_npad].dev = dev;
    LOG("controller %d connected as player %d", dev, g_npad + 1);
    uint32_t mask = 0; for (int i = 0; i <= g_npad; i++) mask |= 1u << i; dm_pad_players(mask);
    return g_npad++;
}
static void pad_push(int i) {
    uint64_t b = g_pad[i].keys | g_pad[i].hat | (g_pad[i].lt > 0.5f ? DM_BTN_ZL : 0) | (g_pad[i].rt > 0.5f ? DM_BTN_ZR : 0);
    if (i == 0) dm_pad_set(b, g_pad[i].lx, g_pad[i].ly, g_pad[i].rx, g_pad[i].ry);
    else dm_pad_set_player(i, b, g_pad[i].lx, g_pad[i].ly, g_pad[i].rx, g_pad[i].ry);
}
static uint64_t key_button(int32_t code) {
    switch (code) {
        case AKEYCODE_BUTTON_B: return DM_BTN_A;   // by position: right
        case AKEYCODE_BUTTON_A: return DM_BTN_B;   // bottom
        case AKEYCODE_BUTTON_Y: return DM_BTN_X;   // top
        case AKEYCODE_BUTTON_X: return DM_BTN_Y;   // left
        case AKEYCODE_BUTTON_L1: return DM_BTN_L;
        case AKEYCODE_BUTTON_R1: return DM_BTN_R;
        case AKEYCODE_BUTTON_L2: return DM_BTN_ZL;
        case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS;
        case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS;
        case AKEYCODE_BUTTON_START: return DM_BTN_PLUS;
        case AKEYCODE_BUTTON_SELECT: return DM_BTN_MINUS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT;
        case AKEYCODE_DPAD_UP: return DM_BTN_UP;
        case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT;
        case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
        default: return 0;
    }
}
static int32_t axis(float v) { if (v > -0.1f && v < 0.1f) v = 0; v = v < -1 ? -1 : v > 1 ? 1 : v; return (int32_t)(v * DM_STICK_MAX); }
static int pad_source(int32_t src) { return (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD; }
static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e), dev = AInputEvent_getDeviceId(e);
    if (type == AINPUT_EVENT_TYPE_KEY && pad_source(src)) {
        uint64_t b = key_button(AKeyEvent_getKeyCode(e)); if (!b) return 0;
        int i = pad_of(dev); if (i < 0) return 1;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_pad[i].keys |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_pad[i].keys &= ~b;
        pad_push(i); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        int i = pad_of(dev); if (i < 0) return 1;
        g_pad[i].lx = axis(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0)); g_pad[i].ly = -axis(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0));   // the Switch's up is +y
        g_pad[i].rx = axis(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0)); g_pad[i].ry = -axis(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));
        float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        if (lt == 0) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0); if (rt == 0) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        g_pad[i].lt = lt; g_pad[i].rt = rt;
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_pad[i].hat = (hx < -0.5f ? DM_BTN_LEFT : 0) | (hx > 0.5f ? DM_BTN_RIGHT : 0) | (hy < -0.5f ? DM_BTN_UP : 0) | (hy > 0.5f ? DM_BTN_DOWN : 0);
        pad_push(i); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        ANativeWindow *w = atomic_load(&g_window); if (!w) return 1;
        float ww = (float)ANativeWindow_getWidth(w), wh = (float)ANativeWindow_getHeight(w);
        int32_t act = AMotionEvent_getAction(e), am = act & AMOTION_EVENT_ACTION_MASK, up = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        dm_touch t[16]; int n = 0; size_t pc = AMotionEvent_getPointerCount(e);
        for (size_t k = 0; k < pc && n < 16; k++) {
            if (am == AMOTION_EVENT_ACTION_UP || am == AMOTION_EVENT_ACTION_CANCEL || (am == AMOTION_EVENT_ACTION_POINTER_UP && (int32_t)k == up)) continue;
            float x = AMotionEvent_getX(e, k) / ww, y = AMotionEvent_getY(e, k) / wh;
            t[n].id = (uint32_t)AMotionEvent_getPointerId(e, k) & 15; t[n].x = x < 0 ? 0 : x >= 1 ? 0.9999f : x; t[n].y = y < 0 ? 0 : y >= 1 ? 0.9999f : y; n++;
        }
        dm_touch_set(t, n); return 1;
    }
    return 0;
}

// ---- Lifecycle ----
static int g_started_once, g_bg;
static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: atomic_store(&g_window, app->window); if (!atomic_load(&g_host_started)) boot(); break;
        case APP_CMD_TERM_WINDOW: atomic_store(&g_window, NULL); break;
        case APP_CMD_GAINED_FOCUS: if (atomic_load(&g_host_started)) dm_focus_set(1); break;
        case APP_CMD_LOST_FOCUS: if (atomic_load(&g_host_started)) dm_focus_set(2); break;
        case APP_CMD_STOP:
            if (atomic_load(&g_host_started) && !g_bg) { g_bg = 1; dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
            break;
        case APP_CMD_START:
            if (g_started_once && g_bg && atomic_load(&g_host_started)) { g_bg = 0; dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); }
            g_started_once = 1; break;
        case APP_CMD_DESTROY: if (atomic_load(&g_host_started)) dm_host_shutdown("activity destroyed"); break;
    }
}
void android_main(struct android_app *app) {
    g_app = app; app->onAppCmd = on_cmd; app->onInputEvent = on_input;
    while (!app->destroyRequested) {
        struct android_poll_source *src = NULL; int events;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0 && src) src->process(app, src);
    }
}
