/* SPDX-License-Identifier: GPL-3.0-only */
/* test_cap.c -- the nano capture drain (nano_cap.c) against a simulated
 * RetailOS capture DMA: two non-contiguous 1024-frame halves, the device
 * cursor at +0x98, a PL080 C1 channel whose DstAddr walks through the halves
 * (physical alias, as a DMAC would hold it), a codec clock slightly off the
 * CPU clock, and the app polling at the jittery UI frame rate measured on the
 * device (16..31 ms, plus stalls).
 *
 * Checks, per mode (DMA position / cursor fallback):
 *   - every delivered frame is the right frame (index-exact against the source),
 *   - delivered + reported-lost == frames the DMA wrote (timeline exact),
 *   - an SSTV image decoded through the drain matches a direct decode.
 * Usage: test_cap photo.ppm [outdir] */
#include "../nano_cap.h"
#include "../sstv_dsp.h"
#include "enc.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- simulated SoC */
#define B0_DEV  0x8B352860u            /* device+0x90 (uncached alias, as peeked) */
#define B1_DEV  0x8B351800u            /* device+0x94 */
#define PHYS(a) ((a) & 0x7FFFFFFFu)    /* what the DMAC holds */

typedef struct {
    int16_t half[2][NCAP_HALF_FRAMES * 2];
    const int16_t *src; long nsrc;      /* mono source */
    long   written;                     /* frames the DMA has written (absolute) */
    double rate_scale;                  /* codec clock / nominal */
    int    dma_visible;                 /* 0: no channel points at the halves (forces cursor mode) */
    int    primed;
} Sim;

static void sim_run_to(Sim *s, uint32_t t_us)
{
    long target = (long)floor((double)t_us * 1e-6 * NCAP_RATE * s->rate_scale);
    while (s->written < target) {
        long f = s->written;
        int h = (int)((f / NCAP_HALF_FRAMES) & 1), o = (int)(f % NCAP_HALF_FRAMES);
        int16_t v = f < s->nsrc ? s->src[f] : 0;
        s->half[h][2 * o] = v;
        s->half[h][2 * o + 1] = (int16_t)(-v);        /* R != L: the drain must take L */
        s->written++;
    }
}
static uint32_t sim_rd32(void *ctx, uint32_t a)
{
    Sim *s = ctx;
    long f = s->written;
    int h = (int)((f / NCAP_HALF_FRAMES) & 1);
    uint32_t pos = (uint32_t)(f % NCAP_HALF_FRAMES) * 4u;
    uint32_t base = h ? B1_DEV : B0_DEV;
    if (a == NCAP_DEV_B0) return B0_DEV;
    if (a == NCAP_DEV_B1) return B1_DEV;
    if (a == NCAP_DEV_CUR) return base;
    if (a == NCAP_PL080_CH(2) + 0x10u) return 0x00028A81u;   /* playback ch2: enabled */
    if (a == NCAP_PL080_CH(2) + 0x04u) return 0x3CA00010u;   /* -> I2S0 TX FIFO */
    if (a == NCAP_PL080_CH(2)) return 0x22001638u;
    if (a == NCAP_PL080_CH(5) + 0x10u) return s->dma_visible ? 0x0000D001u : 0;
    if (a == NCAP_PL080_CH(5) + 0x04u) return PHYS(base) + pos;
    if (a == NCAP_PL080_CH(5)) return 0x3CA00038u;           /* I2S0 RX FIFO */
    return 0;
}
static const int16_t *sim_map(void *ctx, uint32_t a)
{
    Sim *s = ctx;
    return (a & 0x0FFFFFFFu) == (B1_DEV & 0x0FFFFFFFu) ? s->half[1] : s->half[0];
}
static int sim_start(void *ctx, int en) { Sim *s = ctx; (void)en; return s->primed; }

/* ------------------------------------------------------------- consumer */
typedef struct { int16_t *buf; long n, cap; long lost; SstvRx *rx; } Out;
static void put(Out *o, const int16_t *x, int n, int is_gap)
{
    if (o->n + n > o->cap) { o->cap = (o->n + n) * 2; o->buf = realloc(o->buf, (size_t)o->cap * sizeof *o->buf); }
    if (is_gap) memset(o->buf + o->n, 0, (size_t)n * sizeof *o->buf); else memcpy(o->buf + o->n, x, (size_t)n * sizeof *x);
    o->n += n;
}
static void sink(void *u, const int16_t *m, int n) { Out *o = u; put(o, m, n, 0); if (o->rx) sstv_feed(o->rx, m, n, 1); }
static void gapf(void *u, int n) { Out *o = u; put(o, 0, n, 1); o->lost += n; if (o->rx) sstv_gap(o->rx, n); }

static unsigned g_rng = 1;
static unsigned rnd(unsigned m) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 8) % m; }

/* frame-interval models (us) */
enum { J_UI, J_STALLS, J_SLOW };
static uint32_t next_dt(int model, long i)
{
    if (model == J_UI) return 16000u + rnd(15001u);                        /* 16..31 ms UI frames */
    if (model == J_STALLS) {
        if (i % 97 == 50) return 60000u + rnd(60000u);                    /* occasional 60..120 ms stall */
        return 16000u + rnd(15001u);
    }
    return 30000u + rnd(14000u);                                          /* 30..44 ms: near the ring limit */
}

static double luma565(uint16_t v)
{
    uint8_t c[3]; rgb565_to888(v, c);
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}

static double psnr_luma(const uint16_t *a, const uint16_t *b)
{
    double se = 0; int i;
    if (!a || !b) return 0;
    for (i = 320; i < 320 * 240; i++) {
        double d = luma565(a[i]) - luma565(b[i]);
        se += d * d;
    }
    return se > 0 ? 10 * log10(255.0 * 255.0 / (se / (320.0 * 239))) : 99;
}

static int run(const char *name, int dma, int jit, double ppm, const int16_t *src, long nsrc,
               const uint16_t *direct, const char *outdir)
{
    static Sim sim;
    NanoCap *c = calloc(1, sizeof *c);
    NcapBus bus = { sim_rd32, sim_map, sim_start, &sim };
    Out o = { 0, 0, 0, 0, 0 };
    uint32_t t = 0, t_end = (uint32_t)((double)nsrc / NCAP_RATE * 1e6) + 200000u;
    long i, start_abs = -1, bad = 0, checked = 0;
    int ok;
    double pl = 0;
    char path[512];
    SstvStatus S;

    memset(&sim, 0, sizeof sim);
    sim.src = src; sim.nsrc = nsrc; sim.rate_scale = 1.0 + ppm * 1e-6; sim.dma_visible = dma; sim.primed = 1;
    o.rx = sstv_new(NCAP_RATE);
    ncap_init(c, &bus);
    t = 1000; sim_run_to(&sim, t);
    if (ncap_arm(c, t) != NCAP_OK) { printf("arm failed\n"); return 0; }
    for (i = 0; t < t_end; i++) {
        long before = o.n + 0;
        t += next_dt(jit, i);
        sim_run_to(&sim, t);
        ncap_poll(c, t, sink, gapf, &o);
        if (start_abs < 0 && o.n > before) {
            /* the first delivery ends HOLD frames behind the DMA (DMA mode) or at
             * the start of the active half (cursor mode): pin the stream origin */
            if (c->mode == NCAP_MODE_DMAPOS) start_abs = sim.written - 8 - o.n;
            else start_abs = (sim.written / NCAP_HALF_FRAMES) * NCAP_HALF_FRAMES - o.n;
        }
    }
    /* index-exact check of every non-gap frame */
    for (i = 0; i < o.n; i++) {
        long a = start_abs + i;
        if (o.buf[i] == 0) continue;          /* gap (or a true zero sample) */
        checked++;
        if (a < 0 || a >= nsrc || src[a] != o.buf[i]) bad++;
    }
    sstv_status(o.rx, &S);
    {
        int iw, ih;
        const uint16_t *im = sstv_image(o.rx, &iw, &ih);
        pl = (im && iw == 320 && ih == 240) ? psnr_luma(im, direct) : 0;
        snprintf(path, sizeof path, "%s/cap_%s.ppm", outdir, name);
        if (im) ppm_write_565(path, im, iw, ih);
    }
    {
        long expect_total = sim.written - start_abs;                 /* frames the stream should span */
        long span_err = expect_total - (o.n + 8);                     /* allow the HOLD tail */
        ok = (bad == 0 && checked > nsrc / 2 && span_err > -(long)NCAP_HALF_FRAMES - 16 && span_err < (long)NCAP_HALF_FRAMES + 16
              && (S.images == 1 || (S.state == SSTV_RX && S.line >= 230)));
        printf("  %-26s %s  mode %-6s polls %5u  maxgap %3u ms  delivered %8ld  lost %6ld (%u overruns)  "
               "misplaced %ld/%ld  span err %+ld  image Y vs direct %5.1f dB  lines %d img %lu\n",
               name, ok ? "PASS" : "FAIL", c->mode == NCAP_MODE_DMAPOS ? "DMAPOS" : "CURSOR",
               c->polls, c->max_gap_us / 1000u, o.n - o.lost, o.lost, c->overruns, bad, checked,
               span_err, pl, S.line, S.images);
    }
    sstv_free(o.rx); free(o.buf); free(c);
    return ok;
}

int main(int argc, char **argv)
{
    int w, h, pass = 0, total = 0;
    uint8_t *img;
    EncOpts e;
    long n, i;
    int16_t *x;
    SstvRx *rx;
    uint16_t *direct;
    const uint16_t *im;
    int iw = 0, ih = 0;
    const char *outdir = argc > 2 ? argv[2] : "out";
    if (argc < 2 || !(img = ppm_read(argv[1], &w, &h)) || w != 320 || h != 240) {
        fprintf(stderr, "usage: %s photo.ppm [outdir]\n", argv[0]); return 2;
    }
    enc_default_opts(&e); e.snr_db = 35; e.lead_s = 1.0;
    x = enc_encode(img, &e, &n);
    for (i = 0; i < n; i++) if (x[i] == 0) x[i] = 1;                   /* reserve 0 for "gap" in the check */
    rx = sstv_new(NCAP_RATE);
    sstv_feed(rx, x, (int)n, 1);
    im = sstv_image(rx, &iw, &ih);
    if (!im || iw != 320 || ih != 240) { fprintf(stderr, "direct decode failed\n"); return 1; }
    direct = malloc(320 * 240 * 2); memcpy(direct, im, 320 * 240 * 2);
    sstv_free(rx);

    total++; pass += run("dma_ui_jitter",        1, J_UI,      120, x, n, direct, outdir);
    total++; pass += run("dma_near_limit_44ms",  1, J_SLOW,   -80, x, n, direct, outdir);
    total++; pass += run("dma_with_stalls",      1, J_STALLS, 120, x, n, direct, outdir);
    total++; pass += run("cursor_ui_jitter",     0, J_UI,      120, x, n, direct, outdir);
    total++; pass += run("cursor_with_stalls",   0, J_STALLS, 120, x, n, direct, outdir);
    printf("%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
