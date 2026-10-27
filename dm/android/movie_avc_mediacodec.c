// H.264 (AVC) decoding for the movie decoder (dm/host/hle_movie.c) on Android's MediaCodec: the same interface as the VideoToolbox decoder (dm/host/hle_movie_avc.h), which the
// Android build compiles instead. The parameter set handling (each_nal, keep_ps, the avcC record) is the VideoToolbox file's.
// MediaCodec takes Annex B access units with SPS / PPS as csd-0 / csd-1 and hands pictures out in presentation order, so no reorder queue is needed: a picture is copied out of the
// codec's buffer as NV12 at its own size (I420 and NV12 outputs with any stride / slice height; the crop rectangle applied) and waits in a FIFO until mavc_take.
// At the end of the stream (mavc_take with flush and nothing queued) the codec gets its end-of-stream input once and is drained; mavc_drop flushes it for the next stream position.
// RGBA pictures (the native texture mode) are converted here: BT.709 for pictures 720 lines high or more, else BT.601, limited range (VideoToolbox used the stream's own matrix)
#include "../host/hle.h"
#include "../host/hle_movie_avc.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define QMAX 16
enum { COLOR_I420 = 19, COLOR_NV12 = 21, COLOR_FLEX = 0x7F420888 };
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AMediaCodec *mc; int eos_in, eos_out;
    int ow, oh, ostride, oslice, ocolor, cl, ct, cw, ch;   // output buffer layout (format changed) and its crop rectangle
    struct { uint8_t *nv12; int w, h; int64_t pts; } q[QMAX]; int nq, qh;   // FIFO from qh
    int red, rgba;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
static void close_codec(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->eos_in = a->eos_out = 0; }
static void q_clear(mavc *a) { for (int i = 0; i < a->nq; i++) free(a->q[(a->qh + i) % QMAX].nv12); a->nq = a->qh = 0; }
void mavc_drop(mavc *a) { q_clear(a); if (a->mc) { AMediaCodec_flush(a->mc); a->eos_in = a->eos_out = 0; } }
void mavc_free(mavc *a) { if (!a) return; close_codec(a); q_clear(a); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }   // only the conversion at take changes

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: no AVC decoder on this device, outputting black frames"); return 0; }
    uint8_t csd[2][4 + 512]; static const uint8_t sc[4] = {0, 0, 0, 1};
    memcpy(csd[0], sc, 4); memcpy(csd[0] + 4, a->sps, (size_t)a->nsps); memcpy(csd[1], sc, 4); memcpy(csd[1] + 4, a->pps, (size_t)a->npps);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // an upper bound: the decoder takes the size from the SPS
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FLEX);
    AMediaFormat_setBuffer(f, "csd-0", csd[0], 4 + (size_t)a->nsps); AMediaFormat_setBuffer(f, "csd-1", csd[1], 4 + (size_t)a->npps);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: the AVC decoder would not start (%d), outputting black frames", (int)st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    char *name = NULL; AMediaCodec_getName(a->mc, &name);
    LOG("[movie] AVC: MediaCodec %s, profile %d", name ? name : "?", a->nsps > 1 ? a->sps[1] : 0);
    if (name) AMediaCodec_releaseName(a->mc, name);
    a->dirty = 0; return 1;
}
static void out_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v;
    a->ow = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) ? v : 0; a->oh = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v) ? v : 0;
    a->ostride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0 ? v : a->ow;
    a->oslice = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : a->oh;
    a->ocolor = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : COLOR_NV12;
    int32_t l, t, r, b;
    if (AMediaFormat_getRect(f, "crop", &l, &t, &r, &b)) { a->cl = l; a->ct = t; a->cw = r - l + 1; a->ch = b - t + 1; }
    else { a->cl = a->ct = 0; a->cw = a->ow; a->ch = a->oh; }
    AMediaFormat_delete(f);
    LOG("[movie] AVC: %d×%d pictures (buffer %d×%d, stride %d, slice %d, colour format %#x)", a->cw, a->ch, a->ow, a->oh, a->ostride, a->oslice, a->ocolor);
    if (a->ocolor != COLOR_I420 && a->ocolor != COLOR_NV12 && a->ocolor != COLOR_FLEX && !a->red++) LOG("[RED] movie: AVC colour format %#x not known, read as NV12", a->ocolor);
}
// One output buffer → a queued NV12 picture at the crop size
static void keep_picture(mavc *a, const uint8_t *buf, size_t size) {
    int w = a->cw & ~1, h = a->ch & ~1; if (w <= 0 || h <= 0) return;
    size_t ys = (size_t)a->ostride, yplane = ys * (size_t)a->oslice;
    int i420 = a->ocolor == COLOR_I420;
    size_t need = i420 ? yplane + 2 * (ys / 2) * (size_t)(a->oslice / 2) : yplane + ys * (size_t)(a->oslice / 2);
    if (size < need - ys + (size_t)w) { if (!a->red++) LOG("[RED] movie: AVC output buffer of %zu bytes is short for its layout (%zu)", size, need); return; }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[a->qh].nv12); a->qh = (a->qh + 1) % QMAX; a->nq--; }
    uint8_t *o = malloc((size_t)w * h * 3 / 2); if (!o) die("movie: AVC picture allocation failed");
    const uint8_t *y = buf + (size_t)a->ct * ys + (size_t)a->cl;
    for (int r = 0; r < h; r++) memcpy(o + (size_t)r * w, y + r * ys, (size_t)w);
    uint8_t *ouv = o + (size_t)w * h;
    if (i420) {
        size_t cs = ys / 2; const uint8_t *u = buf + yplane + (size_t)(a->ct / 2) * cs + (size_t)(a->cl / 2), *v = u + cs * (size_t)(a->oslice / 2);
        for (int r = 0; r < h / 2; r++) for (int x = 0; x < w / 2; x++) { ouv[(size_t)r * w + 2 * x] = u[r * cs + x]; ouv[(size_t)r * w + 2 * x + 1] = v[r * cs + x]; }
    } else {
        const uint8_t *uv = buf + yplane + (size_t)(a->ct / 2) * ys + (size_t)(a->cl & ~1);
        for (int r = 0; r < h / 2; r++) memcpy(ouv + (size_t)r * w, uv + r * ys, (size_t)w);
    }
    int k = (a->qh + a->nq) % QMAX; a->q[k].nv12 = o; a->q[k].w = w; a->q[k].h = h; a->nq++;
}
static int drain(mavc *a, int64_t timeout_us) {   // take what the codec has; 1 when a picture or the end came out
    int got = 0;
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, got ? 0 : timeout_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { out_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return got;
        size_t cap; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &cap);
        if (b && bi.size > 0) {
            if (!a->ow) out_format(a);
            int before = a->nq; keep_picture(a, b + bi.offset, (size_t)bi.size);
            if (a->nq > before) a->q[(a->qh + a->nq - 1) % QMAX].pts = bi.presentationTimeUs;
            got = 1;
        }
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) { a->eos_out = 1; return 1; }
    }
}
typedef struct { uint8_t *b; size_t len, cap; mavc *a; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(c->a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > c->cap) return;
    c->b[c->len] = 0; c->b[c->len + 1] = 0; c->b[c->len + 2] = 0; c->b[c->len + 3] = 1; memcpy(c->b + c->len + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    // Parameter sets first (they may change the codec), then the access unit straight into the codec's input buffer
    each_nal(p, (size_t)n, ps_only, a);
    if ((a->dirty || !a->mc) && !open_codec(a)) return -1;
    ssize_t i = -1;
    for (int tries = 0; tries < 100 && (i = AMediaCodec_dequeueInputBuffer(a->mc, 10000)) < 0; tries++) drain(a, 0);   // input full: let pictures out meanwhile (1 s at most)
    if (i < 0) { if (!a->red++) LOG("[RED] movie: the AVC decoder takes no input, dropping access units"); return -1; }
    size_t cap; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
    au_ctx c = {b, 0, cap, a}; each_nal(p, (size_t)n, au_add, &c);
    if (c.len < (size_t)n && c.len + 64 < (size_t)n && !a->red++) LOG("[RED] movie: AVC access unit of %d bytes cut to the decoder's %zu-byte input buffer", n, cap);
    AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)pts_us, 0);
    drain(a, 0);
    return 0;
}

// NV12 → RGBA with limited-range BT.709 (or BT.601) coefficients in 16.16 fixed point
static void nv12_rgba(const uint8_t *src, int pw, int ph, uint8_t *dst, int w, int h) {
    int bt709 = ph >= 720; int cr_r = bt709 ? 117489 : 104597, cb_g = bt709 ? 13954 : 25675, cr_g = bt709 ? 34903 : 53279, cb_b = bt709 ? 138438 : 132201;
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    const uint8_t *uvp = src + (size_t)pw * ph;
    for (int r = 0; r < chh; r++) {
        const uint8_t *y = src + (size_t)r * pw, *uv = uvp + (size_t)(r / 2) * pw; uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int yy = (y[x] - 16) * 76309, u = uv[x & ~1] - 128, v = uv[(x & ~1) + 1] - 128;
            int R = (yy + cr_r * v + 32768) >> 16, G = (yy - cb_g * u - cr_g * v + 32768) >> 16, B = (yy + cb_b * u + 32768) >> 16;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc && !a->nq && flush && !a->eos_out) {
        if (!a->eos_in) { ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000); if (i >= 0) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); a->eos_in = 1; } }
        for (int k = 0; k < 20 && !a->nq && !a->eos_out; k++) drain(a, 10000);
    } else if (a->mc && !a->nq) drain(a, 0);
    if (!a->nq) return 0;
    int k = a->qh; uint8_t *pic = a->q[k].nv12; int pw = a->q[k].w, ph = a->q[k].h; if (pts_us) *pts_us = a->q[k].pts;
    a->qh = (a->qh + 1) % QMAX; a->nq--;
    if (a->rgba) nv12_rgba(pic, pw, ph, dst, w, h);
    else {
        int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, pic + (size_t)r * pw, (size_t)cw);
        for (int r = 0; r < chh / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, pic + (size_t)pw * ph + (size_t)r * pw, (size_t)cw);
    }
    free(pic);
    return 1;
}
