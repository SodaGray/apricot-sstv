/* SPDX-License-Identifier: GPL-3.0-only */
/* fuzz_dsp.c -- hours of hostile audio into one decoder, for ASan/UBSan:
 * noise, tone soup, random FSK, silence, clipping, DC, gaps (short and long),
 * aborts/clears, and SSTV pictures of every mode -- truncated, skewed,
 * mistuned, noisy, with and without VIS, sometimes starting mid-picture.
 *   fuzz_dsp <imgdir> <seconds> <seed> [rate] */
#include "../sstv_dsp.h"
#include "enc.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_r;
static unsigned rnd(void) { g_r = g_r * 1103515245u + 12345u; return g_r >> 8; }
static double frand(void) { return (rnd() & 0xFFFFFF) / 16777216.0; }
static int16_t clip(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static const int MODES[] = { 8, 12, 44, 40, 60, 56, 76, 93, 99, 95, 98, 96, 97, 94 };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    double secs = argc > 2 ? atof(argv[2]) : 600;
    int rate = argc > 4 ? atoi(argv[4]) : 44100;
    long total = 0, target;
    SstvRx *s;
    SstvStatus st;
    unsigned long segs = 0, pics = 0;
    g_r = argc > 3 ? (unsigned)atoi(argv[3]) : 1;
    target = (long)(secs * rate);
    FILE *wf = getenv("FUZZWAV") ? fopen(getenv("FUZZWAV"), "wb") : 0;   /* also save the audio */
    long wn = 0;
    s = sstv_new(rate);
    if (!s) return 1;
    if (wf) { static const uint8_t z[44]; fwrite(z, 1, 44, wf); }
    while (total < target) {
        int kind = (int)(rnd() % 10), i;
        long n = 0;
        int16_t *x = 0;
        segs++;
        if (kind <= 2) {                                    /* an SSTV picture, mangled */
            EncOpts o; int w, h; uint8_t *img, *im2; char p[512];
            int m = MODES[rnd() % (sizeof MODES / sizeof MODES[0])];
            enc_default_opts(&o);
            o.mode = m; o.rate = rate;
            o.vis_code = (rnd() % 4) ? m : (rnd() % 2 ? -1 : (int)(rnd() % 128));
            o.skew_ppm = (frand() - 0.5) * ((rnd() % 3) ? 2000 : 60000);
            o.offset_hz = (frand() - 0.5) * 400;
            o.snr_db = (rnd() % 3) ? 3 + frand() * 30 : 100;
            o.lead_s = frand(); o.tail_s = frand();
            o.seed = rnd();
            enc_mode_size(m, &w, &h);
            snprintf(p, sizeof p, "%s/photo%s.ppm", dir, w == 320 && h == 240 ? "" : w == 320 ? "_320x256" : w == 512 ? "_512x400" : w == 800 ? "_800x616" : "_640x496");
            { int iw, ih; img = ppm_read(p, &iw, &ih); if (!img) { fprintf(stderr, "no %s\n", p); return 2; }
              if (iw != w || ih != h) { im2 = rgb_resize(img, iw, ih, w, h); free(img); img = im2; } }
            x = enc_encode(img, &o, &n); free(img);
            if (rnd() % 2) {                                /* keep a random slice */
                long a = (long)(frand() * n * 0.8), b = a + (long)(frand() * (n - a));
                memmove(x, x + a, (size_t)(b - a) * sizeof *x); n = b - a;
            }
            pics++;
        } else {
            n = (long)(frand() * frand() * 20.0 * rate) + 1;
            x = malloc((size_t)n * sizeof *x);
            {
                double amp = (rnd() % 4 ? 1 : 40) * 300 * (frand() + 0.01), f1 = 300 + frand() * 3000, f2 = 1100 + frand() * 1300, ph1 = 0, ph2 = 0, dc = (rnd() % 5 ? 0 : (frand() - 0.5) * 60000);
                long sym = (long)(rate * (0.0005 + frand() * 0.05)), k;
                for (k = 0; k < n; k++) {
                    double v = 0;
                    switch (kind) {
                    case 3: v = amp * (frand() + frand() + frand() - 1.5); break;       /* noise */
                    case 4: v = 0; break;                                                /* silence */
                    case 5: if (k % sym == 0) f2 = 1000 + frand() * 1500;               /* random FSK */
                            ph2 += 2 * M_PI * f2 / rate; v = amp * sin(ph2) + amp * 0.2 * (frand() - 0.5); break;
                    case 6: ph1 += 2 * M_PI * f1 / rate; ph2 += 2 * M_PI * f2 / rate;   /* tone pair */
                            v = amp * (sin(ph1) + sin(ph2)); break;
                    case 7: if (k % sym == 0) f2 = (rnd() % 2) ? 1200 : 1500 + frand() * 800;   /* sync-ish soup */
                            ph2 += 2 * M_PI * f2 / rate; v = amp * sin(ph2); break;
                    case 8: f1 = 1000 + 1500.0 * k / n; ph1 += 2 * M_PI * f1 / rate; v = amp * sin(ph1); break; /* chirp */
                    default: v = (rnd() % 2 ? 32767 : -32768); break;                    /* clipped junk */
                    }
                    x[k] = clip(v + dc);
                }
            }
        }
        for (i = 0; i < n; ) {
            int c = 1 + (int)(rnd() % 4096);
            if (c > n - i) c = (int)(n - i);
            switch (rnd() % 4000) {
            case 0: sstv_gap(s, (int)(rnd() % (3 * rate))); break;
            case 1: sstv_abort(s); break;
            case 2: sstv_clear(s); break;
            case 3: sstv_set_autosync(s, (int)(rnd() % 2)); break;
            case 4: sstv_set_ppm_prior(s, (int)(rnd() % 60000) - 30000); break;
            default: break;
            }
            sstv_feed(s, x + i, c, 1);
            i += c;
            sstv_status(s, &st);
            { int w, h, y0, y1; const uint16_t *im = sstv_image(s, &w, &h); volatile uint16_t acc = 0;
              if (sstv_dirty(s, &y0, &y1) && im) { int y; if (y0 < 0 || y1 >= h || y0 > y1) { fprintf(stderr, "bad dirty %d..%d of %d\n", y0, y1, h); abort(); }
                for (y = y0; y <= y1; y++) acc ^= im[y * w + w - 1]; }
              if (st.line < 0 || (st.height && st.line > st.height)) { fprintf(stderr, "bad line %d/%d\n", st.line, st.height); abort(); } (void)acc; }
        }
        total += n;
        if (wf) { fwrite(x, 2, (size_t)n, wf); wn += n; }
        free(x);
    }
    if (wf) {                                      /* WAV header, mono s16 */
        uint8_t h[44]; uint32_t v; int k;
        memcpy(h, "RIFF", 4); v = (uint32_t)(36 + wn * 2); for (k = 0; k < 4; k++) h[4 + k] = (uint8_t)(v >> (8 * k));
        memcpy(h + 8, "WAVEfmt ", 8); h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
        v = (uint32_t)rate; for (k = 0; k < 4; k++) h[24 + k] = (uint8_t)(v >> (8 * k));
        v = (uint32_t)rate * 2u; for (k = 0; k < 4; k++) h[28 + k] = (uint8_t)(v >> (8 * k));
        h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4);
        v = (uint32_t)(wn * 2); for (k = 0; k < 4; k++) h[40 + k] = (uint8_t)(v >> (8 * k));
        fseek(wf, 0, SEEK_SET); fwrite(h, 1, 44, wf); fclose(wf);
    }
    sstv_status(s, &st);
    printf("seed %u: %.0f s audio, %lu segments, %lu pictures sent, %lu images decoded\n", g_r, (double)total / rate, segs, pics, st.images);
    sstv_free(s);
    return 0;
}
