// AAudio output for hle_audio.c (see au_aaudio.h)
#include "au_aaudio.h"
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)

struct dm_aaudio {
    pthread_mutex_t m;        // stream open / close / start / stop (never taken on the callback thread)
    AAudioStream *s;
    int rate, ch, playing;
    AURenderCallback cb; void *ctx;
    _Atomic int reopening;
};

static aaudio_data_callback_result_t on_data(AAudioStream *s, void *u, void *data, int32_t frames) {
    (void)s; struct dm_aaudio *a = u;
    AudioBufferList bl = {1, {{(UInt32)a->ch, (UInt32)(frames * a->ch * 2), data}}};
    AudioUnitRenderActionFlags f = 0; AudioTimeStamp ts = {0};
    a->cb(a->ctx, &f, &ts, 0, (UInt32)frames, &bl);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *s, void *u, aaudio_result_t err);

static aaudio_result_t open_stream(struct dm_aaudio *a) {
    AAudioStreamBuilder *b; aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, a->rate);
    AAudioStreamBuilder_setChannelCount(b, a->ch);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, on_data, a);
    AAudioStreamBuilder_setErrorCallback(b, on_error, a);
    r = AAudioStreamBuilder_openStream(b, &a->s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { a->s = NULL; return r; }
    // About 5 ms per burst (the device's own burst), with two bursts buffered: the Mac and iOS outputs ask for the same (hle_audio.c au_buffer)
    int32_t burst = AAudioStream_getFramesPerBurst(a->s);
    if (burst > 0) AAudioStream_setBufferSizeInFrames(a->s, 2 * burst);
    LOG("audio device: AAudio %d Hz, %d channels, burst %d frames, buffer %d frames, %s", AAudioStream_getSampleRate(a->s), AAudioStream_getChannelCount(a->s), burst,
        AAudioStream_getBufferSizeInFrames(a->s), AAudioStream_getPerformanceMode(a->s) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low latency" : "normal latency");
    return AAUDIO_OK;
}
static void *reopen(void *p) {
    struct dm_aaudio *a = p;
    pthread_mutex_lock(&a->m);
    if (a->s) { AAudioStream_close(a->s); a->s = NULL; }
    aaudio_result_t r = open_stream(a);
    if (r == AAUDIO_OK && a->playing) r = AAudioStream_requestStart(a->s);
    if (r != AAUDIO_OK) LOG("[RED] audio device: reopening after a disconnect failed (%s); hle_audio's watchdog keeps the silent sink and retries", AAudio_convertResultToText(r));
    else LOG("audio device: reopened after a disconnect");
    atomic_store(&a->reopening, 0);
    pthread_mutex_unlock(&a->m);
    return NULL;
}
static void on_error(AAudioStream *s, void *u, aaudio_result_t err) {
    (void)s; struct dm_aaudio *a = u;
    LOG("audio device: stream error %s", AAudio_convertResultToText(err));
    if (err != AAUDIO_ERROR_DISCONNECTED || atomic_exchange(&a->reopening, 1)) return;
    pthread_t t; if (pthread_create(&t, NULL, reopen, a)) atomic_store(&a->reopening, 0); else pthread_detach(t);
}

OSStatus dm_aaudio_create(AudioUnit *out, int rate, int channels, AURenderCallback cb, void *ctx) {
    struct dm_aaudio *a = calloc(1, sizeof *a);
    pthread_mutex_init(&a->m, NULL); a->rate = rate; a->ch = channels; a->cb = cb; a->ctx = ctx;
    aaudio_result_t r = open_stream(a);
    if (r != AAUDIO_OK) { LOG("[RED] audio device: AAudio open failed (%s)", AAudio_convertResultToText(r)); pthread_mutex_destroy(&a->m); free(a); *out = NULL; return r; }
    *out = a; return noErr;
}
// A stream that is gone (being reopened) reports failure: hle_audio.c's au_go then runs the silent sink and retries
OSStatus AudioOutputUnitStart(AudioUnit a) {
    pthread_mutex_lock(&a->m); a->playing = 1;
    aaudio_result_t r = a->s ? AAudioStream_requestStart(a->s) : AAUDIO_ERROR_DISCONNECTED;
    pthread_mutex_unlock(&a->m); return r;
}
OSStatus AudioOutputUnitStop(AudioUnit a) {
    pthread_mutex_lock(&a->m); a->playing = 0;
    aaudio_result_t r = a->s ? AAudioStream_requestStop(a->s) : AAUDIO_OK;
    pthread_mutex_unlock(&a->m); return r;
}
void dm_aaudio_dispose(AudioUnit a) {
    if (!a) return;
    while (atomic_load(&a->reopening)) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    pthread_mutex_lock(&a->m);
    if (a->s) { AAudioStream_close(a->s); a->s = NULL; }
    pthread_mutex_unlock(&a->m);
    pthread_mutex_destroy(&a->m); free(a);
}
