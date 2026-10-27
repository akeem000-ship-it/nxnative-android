// AAC decoding on MediaCodec (see movie_aac_mediacodec.h): raw frames in, with the AudioSpecificConfig as csd-0; PCM16 out. The codec runs a frame or two behind its input,
// so a packet's samples usually come out with a later packet, as AudioToolbox's start-up delay does (hle_movie.c md_aac takes 0 frames as "none yet")
#include "../host/hle.h"
#include "movie_aac_mediacodec.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

struct maac { AMediaCodec *mc; int ch; int64_t pts; int red; };

maac *maac_new(int rate, int ch, const uint8_t *asc, int asc_n) {
    AMediaCodec *mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!mc) { LOG("[RED] movie: no AAC decoder on this device, outputting silence"); return NULL; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, ch);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    AMediaFormat_setBuffer(f, "csd-0", asc, (size_t)asc_n);
    media_status_t st = AMediaCodec_configure(mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(mc);
    if (st != AMEDIA_OK) { LOG("[RED] movie: the AAC decoder refused %d channels %d Hz (%d), outputting silence", ch, rate, (int)st); AMediaCodec_delete(mc); return NULL; }
    maac *m = calloc(1, sizeof *m); m->mc = mc; m->ch = ch;
    return m;
}
int maac_decode(maac *m, const uint8_t *p, int n, int16_t *out, int cap_frames) {
    ssize_t i = AMediaCodec_dequeueInputBuffer(m->mc, 20000);
    if (i >= 0) {
        size_t cap; uint8_t *b = AMediaCodec_getInputBuffer(m->mc, (size_t)i, &cap);
        size_t k = (size_t)n < cap ? (size_t)n : cap; memcpy(b, p, k);
        AMediaCodec_queueInputBuffer(m->mc, (size_t)i, 0, k, (uint64_t)m->pts, 0); m->pts += 1000;
    } else if (!m->red++) LOG("[RED] movie: the AAC decoder takes no input, dropping a packet");
    int got = 0;
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t o = AMediaCodec_dequeueOutputBuffer(m->mc, &bi, got ? 0 : 2000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (o < 0) break;
        size_t cap; uint8_t *b = AMediaCodec_getOutputBuffer(m->mc, (size_t)o, &cap);
        int frames = bi.size / (2 * m->ch); if (frames > cap_frames - got) { if (!m->red++) LOG("[RED] movie: AAC output larger than the packet slot, cut"); frames = cap_frames - got; }
        if (b && frames > 0) { memcpy(out + (size_t)got * m->ch, b + bi.offset, (size_t)frames * 2 * m->ch); got += frames; }
        AMediaCodec_releaseOutputBuffer(m->mc, (size_t)o, false);
    }
    return got;
}
void maac_free(maac *m) { if (!m) return; AMediaCodec_stop(m->mc); AMediaCodec_delete(m->mc); free(m); }
