/* SPDX-License-Identifier: GPL-3.0-only */
/* test_dsp.c -- SSTV receiver bench: encode every mode with impairments,
 * decode, score against what an ideal decoder would produce. Writes every
 * decoded picture to <outdir>/<case>.ppm.
 * Usage: test_dsp <imgdir> [outdir]   (imgdir has testcard*.ppm, photo*.ppm) */
#include "../sstv_dsp.h"
#include "enc.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAME (-100)                /* vis = the mode's own code */

typedef struct {
    const char *name;
    int  mode, img;               /* VIS code of the mode; 0 = test card, 1 = photo */
    double skew, off, snr, fade_hz, fade_depth;
    int  vis, rate, lines;
    int  drop_every;              /* drop one 1024-frame block every N blocks (0 = never) */
    int  stereo;                  /* feed as interleaved stereo, stride 2 */
    int  ppm_prior;
    /* expectations */
    double min_psnr;              /* luma PSNR, dB */
    int  start;                   /* expected start kind */
    int  images;                  /* expected images completed */
} Case;

static const char *g_imgdir = ".", *g_outdir = "out";

static uint8_t *load_img(int which, int w, int h)
{
    char p[512];
    int iw, ih;
    uint8_t *im;
    if (w == 320 && h == 240) snprintf(p, sizeof p, "%s/%s.ppm", g_imgdir, which ? "photo" : "testcard");
    else snprintf(p, sizeof p, "%s/%s_%dx%d.ppm", g_imgdir, which ? "photo" : "testcard", w, h);
    im = ppm_read(p, &iw, &ih);
    if (!im || iw != w || ih != h) { fprintf(stderr, "missing %s\n", p); exit(2); }
    return im;
}

/* rows ~130..170 (of 240) of the test card are 2..16 px line wedges, mostly
 * above the video bandwidth (grey on any decoder): not scored */
static double psnr_luma(const uint16_t *dec, const uint8_t *ref, int w, int h, int card, int skip_rows)
{
    double se = 0; long n = 0; int x, y;
    for (y = skip_rows; y < h; y++) {
        if (card && y >= 130 * h / 240 && y < 170 * h / 240) continue;
        for (x = 0; x < w; x++) {
            uint8_t c[3]; const uint8_t *r = ref + (y * w + x) * 3;
            double yd, yr;
            rgb565_to888(dec[y * w + x], c);
            yd = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
            yr = 0.299 * r[0] + 0.587 * r[1] + 0.114 * r[2];
            se += (yd - yr) * (yd - yr); n++;
        }
    }
    return se > 0 ? 10 * log10(255.0 * 255.0 / (se / n)) : 99;
}

/* the decoder stores modes wider than SSTV_MAX_W at half size: box-filter
 * the reference to match (in place; updates *w, *h) */
static void ref_fit(uint8_t *ref, int *w, int *h, int dw, int dh)
{
    int x, y, c, W = *w / 2, H = *h / 2;
    if (dw != W || dh != H) return;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) for (c = 0; c < 3; c++) {
        const uint8_t *a = ref + ((2 * y) * *w + 2 * x) * 3 + c, *b = a + *w * 3;
        ref[(y * W + x) * 3 + c] = (uint8_t)((a[0] + a[3] + b[0] + b[3] + 2) / 4);
    }
    *w = W; *h = H;
}

static int run(const Case *c)
{
    EncOpts o;
    long n, i, blocks = 0;
    int16_t *x, *st = 0;
    SstvRx *s;
    SstvStatus S;
    char path[512];
    double pl = 0;
    int ok = 1, blk = 1024, w, h, dw = 0, dh = 0;
    uint8_t *img, *ref;
    const uint16_t *dec;

    enc_mode_size(c->mode, &w, &h);
    img = load_img(c->img, w, h);
    ref = malloc((size_t)w * h * 3);
    enc_reference(c->mode, img, ref);
    enc_default_opts(&o);
    o.mode = c->mode;
    o.rate = c->rate ? c->rate : 44100;
    o.skew_ppm = c->skew; o.offset_hz = c->off; o.snr_db = c->snr ? c->snr : 100;
    o.fade_hz = c->fade_hz; o.fade_depth = c->fade_depth;
    o.vis_code = c->vis == SAME ? c->mode : c->vis; o.lines = c->lines;
    x = enc_encode(img, &o, &n);

    s = sstv_new(o.rate);
    if (!s) { printf("  %-24s OOM\n", c->name); return 0; }
    if (c->ppm_prior) sstv_set_ppm_prior(s, c->ppm_prior);
    if (c->stereo) { st = malloc((size_t)n * 2 * sizeof *st); for (i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = x[i]; }
    for (i = 0; i < n; i += blk) {
        int m = (int)(n - i < blk ? n - i : blk);
        blocks++;
        if (c->drop_every && blocks % c->drop_every == 0) { sstv_gap(s, m); continue; }
        if (c->stereo) sstv_feed(s, st + 2 * i, m, 2); else sstv_feed(s, x + i, m, 1);
    }
    sstv_status(s, &S);
    dec = sstv_image(s, &dw, &dh);
    ref_fit(ref, &w, &h, dw, dh);
    if (dec && dw == w && dh == h) {
        pl = psnr_luma(dec, ref, w, h, c->img == 0, c->mode == 8 ? 1 : 2);
        snprintf(path, sizeof path, "%s/%s.ppm", g_outdir, c->name);
        ppm_write_565(path, dec, w, h);
    }
    if (c->min_psnr > 0 && pl < c->min_psnr) ok = 0;
    if (c->start >= 0 && S.start_kind != c->start) ok = 0;
    if (c->images >= 0 && (int)S.images != c->images) ok = 0;
    if (c->images > 0 && S.ppm_valid && fabs(S.ppm - c->skew) > 40 + fabs(c->skew) * 0.01) ok = 0;
    if (c->images > 0 && (!S.mode || strcmp(S.mode, enc_mode_name(c->mode)))) ok = 0;
    printf("  %-24s %s  %-10s Y %5.1f dB  rows %3d/%3d  start %d end %d  vis %3d  off %+4d  "
           "ppm %+6d%s (true %+6.0f)  sync %3d/%3d  redraw %2d  img %lu\n",
           c->name, ok ? "PASS" : "FAIL", S.mode ? S.mode : "-", pl, S.line, S.height, S.start_kind, S.end_kind,
           S.last_vis, S.offset_hz, S.ppm, S.ppm_valid ? "" : "?", c->skew, S.sync_good, S.sync_seen,
           S.slant_redraws, S.images);
    sstv_free(s);
    free(x); free(st); free(img); free(ref);
    return ok;
}

/* 6 minutes of things that are not SSTV: white noise, silence, random
 * 1100/1200/1300/1500/1900/2300 Hz FSK in 10-60 ms chips, a steady 1900 Hz
 * tone. Nothing may start, in any mode. */
static int run_false_starts(void)
{
    static const int fr[] = { 1100, 1200, 1300, 1500, 1900, 2300 };
    long n = 44100L * 360, i;
    int16_t *x = malloc((size_t)n * sizeof *x);
    unsigned st = 12345;
    double ph = 0, f = 1900;
    long chip = 0;
    SstvRx *s = sstv_new(44100);
    SstvStatus S;
    int ok;
    for (i = 0; i < n; i++) {
        double t = (double)i / 44100, v;
        st = st * 1103515245u + 12345u;
        if (t < 120) v = ((int)(st >> 16) % 16000) - 8000;            /* noise */
        else if (t < 150) v = 0;                                        /* silence */
        else if (t < 330) {                                             /* random FSK */
            if (i >= chip) { f = fr[(st >> 20) % 6]; chip = i + 441 + (long)((st >> 8) % 2205); }
            ph += 2 * M_PI * f / 44100; v = 9000 * sin(ph);
        } else { ph += 2 * M_PI * 1900.0 / 44100; v = 9000 * sin(ph); } /* steady leader-like tone */
        x[i] = (int16_t)v;
    }
    for (i = 0; i < n; i += 1024) sstv_feed(s, x + i, (int)(n - i < 1024 ? n - i : 1024), 1);
    sstv_status(s, &S);
    ok = (S.start_kind == SSTV_START_NONE && S.images == 0 && S.state == SSTV_IDLE);
    printf("  %-24s %s  6 min noise/silence/random FSK/1900 Hz: start %d (%s) vis %d images %lu\n",
           "false_starts", ok ? "PASS" : "FAIL", S.start_kind, S.mode ? S.mode : "-", S.last_vis, S.images);
    sstv_free(s); free(x);
    return ok;
}

/* Martin M1, then Robot 36, then PD90 on one receiver, 3 s apart: all three
 * decode, each in its own mode */
static int run_sequence(void)
{
    static const int modes[3] = { 44, 8, 99 };
    EncOpts o;
    long n = 0, nb, i;
    int16_t *a = 0, *b;
    SstvRx *s = sstv_new(44100);
    SstvStatus S;
    int k, ok = 1, w, h, iw, ih;
    unsigned long seen = 0;
    const char *got[3] = { 0, 0, 0 };
    double pl[3] = { 0, 0, 0 };
    for (k = 0; k < 3; k++) {
        uint8_t *img;
        enc_mode_size(modes[k], &w, &h);
        img = load_img(1, w, h);
        enc_default_opts(&o); o.mode = o.vis_code = modes[k]; o.skew_ppm = 700; o.tail_s = 3.0; o.snr_db = 30; o.seed = 99 + k;
        b = enc_encode(img, &o, &nb);
        if (!a) { a = b; n = nb; } else { a = enc_concat(a, n, b, nb, &n); free(b); }
        free(img);
    }
    for (i = 0; i < n; i += 1024) {
        sstv_feed(s, a + i, (int)(n - i < 1024 ? n - i : 1024), 1);
        sstv_status(s, &S);
        if (S.images != seen && seen < 3) {
            uint8_t *img, *ref;
            const uint16_t *d = sstv_image(s, &iw, &ih);
            got[seen] = S.mode;
            enc_mode_size(modes[seen], &w, &h);
            img = load_img(1, w, h); ref = malloc((size_t)w * h * 3);
            enc_reference(modes[seen], img, ref);
            ref_fit(ref, &w, &h, iw, ih);
            if (iw == w && ih == h) pl[seen] = psnr_luma(d, ref, w, h, 0, 2);
            free(img); free(ref);
            seen = S.images;
        }
    }
    for (k = 0; k < 3; k++)
        if (!got[k] || strcmp(got[k], enc_mode_name(modes[k])) || pl[k] < 28) ok = 0;
    printf("  %-24s %s  %s %.1f dB / %s %.1f dB / %s %.1f dB  ppm %+d\n", "sequence_M1_R36_PD90", ok ? "PASS" : "FAIL",
           got[0] ? got[0] : "-", pl[0], got[1] ? got[1] : "-", pl[1], got[2] ? got[2] : "-", pl[2], S.ppm);
    sstv_free(s); free(a);
    return ok;
}

/* A picture hit mid-way by trouble, as on the nano: kind 0 = capture gap of
 * dur seconds (audio lost, time passes, sstv_gap), 1 = fade (dur seconds of
 * noise only, longer than the lost timeout), 2 = sstv_abort then go on.
 * Must end as ONE image, the row counter must never jump back to the top,
 * and the rows outside the damaged band must decode. */
static int run_dropout(const char *name, int mode, double line_s, int kind, double at, double dur)
{
    EncOpts o;
    int w, h, dw, dh, restarts = 0, last_line = 0, ok = 1, sw, sh;
    long n, i, a, b;
    uint8_t *img, *ref;
    int16_t *x;
    SstvRx *s;
    SstvStatus S;
    const uint16_t *dec;
    double se = 0, t0, rows_per_s; long cnt = 0; int y, xx, ra, rb;
    char path[512];
    enc_mode_size(mode, &w, &h);
    img = load_img(1, w, h);
    ref = malloc((size_t)w * h * 3); enc_reference(mode, img, ref);
    enc_default_opts(&o); o.mode = o.vis_code = mode; o.skew_ppm = 150; o.snr_db = 25; o.seed = 7;
    x = enc_encode(img, &o, &n);
    s = sstv_new(o.rate);
    a = (long)(at * n); b = a + (long)(dur * o.rate);
    if (kind == 1) { unsigned r = 5; for (i = a; i < b && i < n; i++) { r = r * 1103515245u + 12345u; x[i] = (int16_t)(((int)(r >> 16) & 0x3FFF) - 0x2000); } }
    for (i = 0; i < n; ) {
        int c = 1024;
        if (kind == 0 && i >= a && i < b) { sstv_gap(s, (int)(b - i)); i = b; continue; }
        if (kind == 2 && i < a && i + c > a) c = (int)(a - i);
        if (c > n - i) c = (int)(n - i);
        sstv_feed(s, x + i, c, 1);
        i += c;
        if (kind == 2 && i == a) sstv_abort(s);
        sstv_status(s, &S);
        if (S.state == SSTV_RX && S.line + 8 < last_line) restarts++;
        if (S.state == SSTV_RX) last_line = S.line;
    }
    sstv_status(s, &S);
    dec = sstv_image(s, &dw, &dh);
    sw = w; sh = h; ref_fit(ref, &sw, &sh, dw, dh);
    /* rows outside [row(a) - 2 lines, row(b) + 8 lines] */
    t0 = (double)n - o.tail_s * o.rate - (double)h / (mode >= 93 || mode == 94 ? 2 : 1) * line_s * o.rate;
    rows_per_s = (double)sh / ((double)h / (mode >= 93 || mode == 94 ? 2 : 1) * line_s);
    ra = (int)(((a - t0) / o.rate - 2 * line_s) * rows_per_s);
    rb = (int)(((b - t0) / o.rate + 8 * line_s) * rows_per_s);
    if (dec && dw == sw && dh == sh)
        for (y = 2; y < sh; y++) {
            if (y >= ra && y <= rb) continue;
            for (xx = 0; xx < sw; xx++) {
                uint8_t c[3]; const uint8_t *r = ref + (y * sw + xx) * 3; double yd, yr;
                rgb565_to888(dec[y * sw + xx], c);
                yd = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; yr = 0.299 * r[0] + 0.587 * r[1] + 0.114 * r[2];
                se += (yd - yr) * (yd - yr); cnt++;
            }
        }
    {
        double pl = cnt && se > 0 ? 10 * log10(255.0 * 255.0 / (se / cnt)) : 0;
        if (S.images != 1 || restarts || S.line != h || pl < 24 || !S.mode || strcmp(S.mode, enc_mode_name(mode))) ok = 0;
        if (kind != 0 && S.relocks < 1) ok = 0;
        printf("  %-24s %s  %-10s Y %5.1f dB outside rows %d..%d  rows %3d/%3d  img %lu  relocks %d  restarts %d  end %d\n",
               name, ok ? "PASS" : "FAIL", S.mode ? S.mode : "-", pl, ra, rb, S.line, h, S.images, S.relocks, restarts, S.end_kind);
    }
    if (dec) { snprintf(path, sizeof path, "%s/%s.ppm", g_outdir, name); ppm_write_565(path, dec, dw, dh); }
    sstv_free(s); free(x); free(img); free(ref);
    return ok;
}

int main(int argc, char **argv)
{
    int i, pass = 0, total = 0;
    static const Case cases[] = {
        /* name                  mode img skew   off  snr  fhz fdep vis   rate lines drop st prior minpsnr start img */
        /* ---- Robot 36 (the original bench) ---- */
        { "r36_clean",              8, 0,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    30, 1, 1 },
        { "r36_clean_photo",        8, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    35, 1, 1 },
        { "r36_stereo_stride2",     8, 0,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 1, 0,    30, 1, 1 },
        { "r36_skew_+5000ppm",      8, 0, 5000,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    29, 1, 1 },
        { "r36_skew_-8000ppm",      8, 0,-8000,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    29, 1, 1 },
        { "r36_skew_+20000ppm",     8, 0,20000,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    28, 1, 1 },
        { "r36_offset_-120Hz",      8, 0,    0, -120,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    30, 1, 1 },
        { "r36_snr_15dB",           8, 1,  300,    0,  15,  0,  0, SAME, 0,     0,    0, 0, 0,    20, 1, 1 },
        { "r36_snr_10dB",           8, 1,  300,   30,  10,  0,  0, SAME, 0,     0,    0, 0, 0,    15, 1, 1 },
        { "r36_snr_7dB",            8, 1,  300,   30,   7,  0,  0, SAME, 0,     0,    0, 0, 0,    11, 1, 1 },
        { "r36_fading_0.5Hz",       8, 1,  200,    0,  20,0.5,0.9, SAME, 0,     0,    0, 0, 0,    16, 1, 1 },
        { "r36_no_vis_autosync",    8, 0,  400,    0,   0,  0,  0,   -1, 0,     0,    0, 0, 0,    29, 2, 1 },
        { "r36_no_vis_12dB",        8, 1,  400,   50,  12,  0,  0,   -1, 0,     0,    0, 0, 0,    16, 2, 1 },
        { "r36_gaps_1_in_40",       8, 0,  200,    0,  30,  0,  0, SAME, 0,     0,   40, 0, 0,    23, 1, 1 },
        { "r36_rate_48000",         8, 0,  300,    0,   0,  0,  0, SAME, 48000, 0,    0, 0, 0,    29, 1, 1 },
        { "r36_rate_8000",          8, 0,  300,    0,   0,  0,  0, SAME, 8000,  0,    0, 0, 0,    28, 1, 1 },
        { "r36_half_image",         8, 0,    0,    0,   0,  0,  0, SAME, 0,   120,    0, 0, 0,     0, 1, 0 },
        { "r36_prior_wrong_3000",   8, 0,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 3000, 30, 1, 1 },
        /* ---- every other mode: clean, impaired, and without its VIS ---- */
        { "r72_clean",             12, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    33, 1, 1 },
        { "r72_skew_snr20",        12, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    24, 1, 1 },
        { "r72_no_vis",            12, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
        { "m1_clean",              44, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    33, 1, 1 },
        { "m1_card",               44, 0,  300,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    29, 1, 1 },
        { "m1_skew_snr20",         44, 1, -400,  -40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    24, 1, 1 },
        { "m1_no_vis",             44, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
        { "m2_clean",              40, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "m2_skew_snr20",         40, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    22, 1, 1 },
        { "m2_no_vis",             40, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    27, 2, 1 },
        { "s1_clean",              60, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    33, 1, 1 },
        { "s1_card",               60, 0,  300,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    29, 1, 1 },
        { "s1_skew_snr20",         60, 1, -400,  -40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    24, 1, 1 },
        { "s1_no_vis",             60, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
        { "s2_clean",              56, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "s2_skew_snr20",         56, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    22, 1, 1 },
        { "s2_no_vis",             56, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    27, 2, 1 },
        { "sdx_clean",             76, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    33, 1, 1 },
        { "sdx_skew_snr20",        76, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    25, 1, 1 },
        { "pd50_clean",            93, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "pd50_skew_snr20",       93, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    22, 1, 1 },
        { "pd50_no_vis",           93, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    27, 2, 1 },
        { "pd90_clean",            99, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    33, 1, 1 },
        { "pd90_skew_snr20",       99, 1, -400,  -40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    24, 1, 1 },
        { "pd90_no_vis",           99, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
        { "pd120_clean",           95, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    30, 1, 1 },
        { "pd120_card",            95, 0,  300,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    25, 1, 1 },
        { "pd120_skew_snr20",      95, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    20, 1, 1 },
        { "pd120_no_vis",          95, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    26, 2, 1 },
        { "pd160_clean",           98, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "pd160_skew_snr20",      98, 1, -400,  -40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    23, 1, 1 },
        { "pd180_clean",           96, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "pd180_skew_snr20",      96, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    22, 1, 1 },
        { "pd240_clean",           97, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "pd240_skew_snr20",      97, 1,  400,   40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    23, 1, 1 },
        { "pd240_no_vis",          97, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
        { "pd290_clean",           94, 1,    0,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    32, 1, 1 },
        { "pd290_card",            94, 0,  300,    0,   0,  0,  0, SAME, 0,     0,    0, 0, 0,    25, 1, 1 },
        { "pd290_skew_snr20",      94, 1, -400,  -40,  20,  0,  0, SAME, 0,     0,    0, 0, 0,    23, 1, 1 },
        { "pd290_no_vis",          94, 1,  300,    0,  30,  0,  0,   -1, 0,     0,    0, 0, 0,    28, 2, 1 },
    };
    if (argc > 1) g_imgdir = argv[1];
    if (argc > 2) g_outdir = argv[2];
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) { total++; pass += run(&cases[i]); }
    total++; pass += run_false_starts();
    total++; pass += run_sequence();
    total++; pass += run_dropout("pd290_gap_15s",   94, 0.93728, 0, 0.5, 15.0);
    total++; pass += run_dropout("pd290_fade_20s",  94, 0.93728, 1, 0.5, 20.0);
    total++; pass += run_dropout("pd290_abort_mid", 94, 0.93728, 2, 0.5, 0.0);
    total++; pass += run_dropout("pd120_fade_15s",  95, 0.50848, 1, 0.4, 15.0);
    total++; pass += run_dropout("m1_fade_14s",     44, 0.446446, 1, 0.6, 14.0);
    total++; pass += run_dropout("s1_gap_5s",       60, 0.42822, 0, 0.3, 5.0);
    printf("%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
