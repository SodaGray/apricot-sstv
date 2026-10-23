/* SPDX-License-Identifier: GPL-3.0-only */
/* preview_app.c -- run the real sstv_app.c on the host: stub SDK with a
 * 240x432 framebuffer, a simulated capture DMA streaming an SSTV signal at
 * real-time pace, ~30 ms frames with jitter, scripted taps. Dumps the screen
 * (portrait framebuffer, as the iPod holds it) at a few moments and checks the
 * BMP the app saves. Usage: preview_app photo.ppm outdir */
#include "hb_sdk.h"
#include "hb_raw_surface.h"
#include "../nano_cap.h"
#include "enc.h"
#include "sstv_dsp.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static double fsum_t; static long fn_t; static double ftop[8]; static long ftopf[8];

#define FW 240
#define FH 432
static uint32_t fbs[2][FW * FH], *fb = fbs[0], before[FW * FH];
static int fbmode;                    /* 0 persistent, 1 double-buffered, 2 cleared every frame */
static long writes_sum, frames_n;
static void frame(const hb_spoint_t *t)
{
    int i; long ch = 0;
    if (fbmode == 1) fb = (fb == fbs[0]) ? fbs[1] : fbs[0];
    if (fbmode == 2) for (i = 0; i < FW * FH; i++) fb[i] = 0;
    for (i = 0; i < FW * FH; i++) before[i] = fb[i];
    {   double a = now_s(), d; int k, j; hb_raw_frame(t); d = now_s() - a; fsum_t += d; fn_t++;
        if (getenv("SLOWLOG") && d * 1e3 > atof(getenv("SLOWLOG"))) printf("  SLOW frame %ld: %.1f ms\n", fn_t, d * 1e3);
        for (k = 0; k < 8 && d <= ftop[k]; k++) ;
        if (k < 8) { for (j = 7; j > k; j--) { ftop[j] = ftop[j-1]; ftopf[j] = ftopf[j-1]; } ftop[k] = d; ftopf[k] = fn_t; } }
    for (i = 0; i < FW * FH; i++) ch += fb[i] != before[i];
    writes_sum += ch; frames_n++;
}
static uint64_t g_us = 5000000;
uint32_t *hb_raw_fb(void) { return fb; }
int hb_raw_w(void) { return FW; }
int hb_raw_h(void) { return FH; }
void hb_raw_fill(uint32_t c) { int i; for (i = 0; i < FW * FH; i++) fb[i] = c; }
void hb_raw_fill_rect(int x, int y, int w, int h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void hb_raw_rect_outline(int x, int y, int w, int h, int t, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)t; (void)c; }
uint64_t hb_time_uptime_us(void) { return g_us; }
uint32_t hb_time_uptime_ms(void) { return (uint32_t)(g_us / 1000); }
void hb_trace_init(void) {}
void hb_trace_log(const char *tag, uint32_t a, uint32_t b) { printf("  trace %-8s %u %u\n", tag, a, b); }
static FILE *g_f; static char g_dir[256]; static char g_last[512];
int hb_fs_stream_open(const char *p) { snprintf(g_last, sizeof g_last, "%s%s", g_dir, p); g_f = fopen(g_last, "wb"); return g_f != 0; }
int hb_fs_stream_write(const void *b, uint32_t n) { return g_f && fwrite(b, 1, n, g_f) == n; }
int hb_fs_stream_close(void) { int ok = g_f && fclose(g_f) == 0; g_f = 0; return ok; }
/* OS heap with accounting (live bytes, peak, count) */
static long heap_live, heap_peak, heap_n;
void *hb_os_alloc(uint32_t n) { uint32_t *p = malloc(n + 8u); if (!p) return 0; p[0] = n; heap_live += n; heap_n++; if (heap_live > heap_peak) heap_peak = heap_live; return p + 2; }
void hb_os_free(void *q) { uint32_t *p; if (!q) return; p = (uint32_t *)q - 2; heap_live -= p[0]; heap_n--; free(p); }
/* the OS heap's free space: HEAPFREE (bytes, default 3 MB) minus what we hold */
uint32_t hb_os_heap_free(void) { long f = (getenv("HEAPFREE") ? atol(getenv("HEAPFREE")) : 3L << 20) - heap_live; return f > 0 ? (uint32_t)f : 0u; }
uint32_t sstv_pool_peak(void);

/* simulated capture: DMA writes the source at 44100 into two halves */
static int16_t H[2][2048]; static const int16_t *S; static long NS; static long W; static uint64_t t0;
static double g_codec = 44100;    /* the simulated codec's true rate */
static void run_dma(void) { long target = (long)((double)(g_us - t0) * g_codec * 1e-6); while (W < target) { int h = (int)((W / 1024) & 1), o = (int)(W % 1024); int16_t v = W < NS ? S[W] : 0; H[h][2*o] = v; H[h][2*o+1] = v; W++; } }
static uint32_t rd(void *c, uint32_t a) { (void)c; int h = (int)((W / 1024) & 1);
    if (a == NCAP_DEV_B0) return 0x8B352860u; if (a == NCAP_DEV_B1) return 0x8B351800u;
    if (a == NCAP_DEV_CUR) return h ? 0x8B351800u : 0x8B352860u;
    if (a == NCAP_PL080_CH(3) + 0x10u) return 1; if (a == NCAP_PL080_CH(3)) return 0x3CA00038u;
    if (a == NCAP_PL080_CH(3) + 4u) return ((h ? 0x8B351800u : 0x8B352860u) & 0x7FFFFFFFu) + (uint32_t)(W % 1024) * 4u;
    return 0; }
static const int16_t *mp(void *c, uint32_t a) { (void)c; return (a & 0xFFFFF) == (0x8B351800u & 0xFFFFF) ? H[1] : H[0]; }
static int ds(void *c, int e) { (void)c; (void)e; return 1; }
static const NcapBus bus = { rd, mp, ds, 0 };
const NcapBus *ncap_nano_bus(void) { return &bus; }

static void dump(const char *dir, const char *name)
{
    char p[512]; uint8_t *b = malloc(FW * FH * 3); int i;
    for (i = 0; i < FW * FH; i++) { b[i*3] = fb[i] >> 16; b[i*3+1] = fb[i] >> 8; b[i*3+2] = fb[i]; }
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name); ppm_write(p, b, FW, FH); free(b);
}
/* tap at landscape (lx, ly) -> portrait touch for rotation 0 */
static hb_spoint_t tap(int lx, int ly) { hb_spoint_t t; t.x = (int16_t)(FW - 1 - ly); t.y = (int16_t)lx; t.down = 1; return t; }

int main(int argc, char **argv)
{
    int w, h, f; long n; uint8_t *img; EncOpts o; hb_spoint_t up = { 0, 0, 0 }, t;
    unsigned r = 7;
    if (argc < 3 || !(img = ppm_read(argv[1], &w, &h))) { fprintf(stderr, "usage: preview_app img.ppm outdir [codec_rate]\n"); return 2; }
    if (argc > 3) g_codec = atof(argv[3]);
    if (getenv("FBMODE")) fbmode = atoi(getenv("FBMODE"));
    snprintf(g_dir, sizeof g_dir, "%s", argv[2]);
    enc_default_opts(&o); o.lead_s = 3.5; o.rate = (int)g_codec;
    if (getenv("MODE")) {
        int mw, mh; o.mode = o.vis_code = atoi(getenv("MODE"));
        enc_mode_size(o.mode, &mw, &mh);
        if (w != mw || h != mh) { uint8_t *r = rgb_resize(img, w, h, mw, mh); free(img); img = r; }
    } o.snr_db = 28; o.skew_ppm = 180; o.offset_hz = 25;
    S = enc_encode(img, &o, &n); NS = n;
    if (getenv("SEQ")) {                     /* several pictures back to back: SEQ="8,44,95" */
        char buf[256], *tok; int16_t *acc = 0; long na = 0;
        snprintf(buf, sizeof buf, "%s", getenv("SEQ"));
        for (tok = strtok(buf, ","); tok; tok = strtok(0, ",")) {
            int mw, mh; uint8_t *ri; int16_t *x; long nx; EncOpts q = o;
            q.mode = q.vis_code = atoi(tok); q.lead_s = 2.0; q.tail_s = 1.0;
            if (!enc_mode_size(q.mode, &mw, &mh)) continue;
            ri = rgb_resize(img, w, h, mw, mh);
            x = enc_encode(ri, &q, &nx); free(ri);
            acc = enc_concat(acc, na, x, nx, &na); free(x);
            printf("  seq: %s %.0f s\n", enc_mode_name(q.mode), (double)nx / q.rate);
        }
        S = acc; NS = n = na;
    }
    if (getenv("WAVIN")) {                   /* any WAV as the "mic" (first channel) */
        long fr; int ch, wr; int16_t *x = wav_read(getenv("WAVIN"), &fr, &ch, &wr), *m;
        long k;
        if (!x) { fprintf(stderr, "can't read %s\n", getenv("WAVIN")); return 2; }
        m = malloc((size_t)fr * sizeof *m);
        for (k = 0; k < fr; k++) m[k] = x[k * ch];
        free(x); S = m; NS = n = fr;
        printf("  wavin: %.0f s\n", (double)fr / wr);
    }
    hb_raw_init(FW, FH);
    frame(&up);
    dump(argv[2], "ui_0_idle");
    t0 = g_us; W = 0;
    t = tap(377, 175); frame(&t); frame(&up);   /* LISTEN */
    t = tap(360, 200); frame(&t); frame(&up);   /* REC (12 s raw capture) */
    for (f = 0; f < 200000; f++) {
        r = r * 1103515245u + 12345u;
        g_us += 16000 + (r >> 8) % 15001;      /* 16..31 ms UI frames, as on the device */
        run_dma();
        if (getenv("TAPS") && (r >> 4) % 400 == 0) {   /* random button presses (not Rotate) */
            static const int bx[5] = { 340, 377, 413, 350, 377 }, by[5] = { 200, 200, 200, 222, 175 };
            int b = (int)((r >> 12) % 5);
            t = tap(bx[b], by[b]); frame(&t);
        }
        frame(&up);
        if (f == (getenv("DUMPF") ? atoi(getenv("DUMPF")) : 800)) dump(argv[2], "ui_1_receiving");
        if ((double)(g_us - t0) / 1e6 > (double)n / g_codec + 1.0) break;
    }
    {   int k; printf("frame time: avg %.3f ms; slowest:", fsum_t / fn_t * 1e3);
        for (k = 0; k < 8; k++) printf(" %.1f ms@%ld", ftop[k] * 1e3, ftopf[k]); printf("\n"); }
    printf("heap: live %ld B in %ld blocks, peak %ld B\n", heap_live, heap_n, heap_peak);
    printf("fbmode %d: %ld frames, avg %.0f pixels changed per frame\n", fbmode, frames_n, (double)writes_sum / frames_n);
    dump(argv[2], "ui_2_done");
    printf("autosaved: %s\n", g_last);
    t = tap(377, 175); frame(&t); frame(&up);   /* STOP: frees the decoder */
    g_us += 1000000; frame(&up);
    dump(argv[2], "ui_3_stopped");
    t = tap(340, 200); frame(&t); frame(&up);   /* SAVE the stopped picture */
    printf("saved after stop: %s\n", g_last);
    printf("heap after stop: live %ld B in %ld blocks, peak %ld B\n", heap_live, heap_n, heap_peak);
    printf("decoder pool peak: %u of %u bytes\n", sstv_pool_peak(), (unsigned)SSTV_MEM_BYTES);
    {   int bw, bh; FILE *fp = fopen(g_last, "rb"); unsigned char hd[54];
        if (fp && fread(hd, 1, 54, fp) == 54) { bw = hd[18] | hd[19] << 8; bh = hd[22] | hd[23] << 8; printf("BMP %dx%d %d bpp\n", bw, bh, hd[28]); }
        if (fp) fclose(fp); }
    return 0;
}
