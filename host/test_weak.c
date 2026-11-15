/* SPDX-License-Identifier: GPL-3.0-only */
#include "../sstv_dsp.h"
#include "enc.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int sstv_pool_open(void);
void sstv_pool_close(void);
uint32_t sstv_pool_peak(void);
static int heap_blocks;
void *hb_os_alloc(uint32_t n) { void *p = malloc(n); if (p) heap_blocks++; return p; }
void hb_os_free(void *p) { if (p) { heap_blocks--; free(p); } }

static void memory(void)
{
    static const int rates[] = {8000, 12000, 16537, 16538, 27562, 27563, 38587, 38588, 44100, 48000};
    int r;
    for (r = 0; r < (int)(sizeof rates / sizeof rates[0]); r++) {
        int16_t x[4096] = {0};
        SstvRx *s;
        assert(sstv_pool_open());
        s = sstv_new(rates[r]); assert(s);
        assert(sstv_set_weak(s, 1)); assert(sstv_set_weak(s, 0)); assert(sstv_set_weak(s, 1));
        sstv_feed(s, x, 4096, 1);
        assert(!sstv_set_weak(s, 0));
        sstv_gap(s, rates[r] / 2);
        sstv_feed(s, x, 4096, 1);
        sstv_free(s); sstv_pool_close();
        assert(heap_blocks == 0 && sstv_pool_peak() <= SSTV_MEM_BYTES);
    }
    printf("pool peak: %u/%u bytes across rate boundaries\n", sstv_pool_peak(), (unsigned)SSTV_MEM_BYTES);
}

static void image(int mode, int rate, int no_vis, double snr, double off, double fade)
{
    int w, h, i, iw, ih;
    uint8_t *rgb;
    int16_t *x;
    long n, at;
    EncOpts o;
    SstvRx *s;
    SstvStatus st;
    assert(enc_mode_size(mode, &w, &h));
    rgb = malloc((size_t)w*h*3); assert(rgb);
    for (i = 0; i < w*h; i++) {
        rgb[3*i] = (uint8_t)((i % w)*255/(w-1));
        rgb[3*i+1] = (uint8_t)((i / w)*255/(h-1));
        rgb[3*i+2] = (uint8_t)(255-rgb[3*i]);
    }
    enc_default_opts(&o);
    o.mode = mode; o.vis_code = no_vis ? -1 : mode; o.rate = rate;
    o.snr_db = snr; o.offset_hz = off; o.amplitude = 4000;
    o.fade_hz = fade ? .7 : 0; o.fade_depth = fade;
    x = enc_encode(rgb, &o, &n); assert(x);
    assert(sstv_pool_open()); s = sstv_new(rate); assert(s && sstv_set_weak(s, 1));
    for (at = 0; at < n; at += 1024) sstv_feed(s, x+at, (int)(n-at < 1024 ? n-at : 1024), 1);
    sstv_status(s, &st);
    assert(st.mode && !strcmp(st.mode, enc_mode_name(mode)) && st.end_kind == SSTV_END_COMPLETE);
    assert(st.line == h && st.images == 1);
    assert(st.start_kind == (no_vis ? SSTV_START_SYNC : SSTV_START_VIS));
    assert(sstv_image(s, &iw, &ih) && iw <= SSTV_MAX_W && ih <= SSTV_MAX_H);
    sstv_free(s); sstv_pool_close(); assert(heap_blocks == 0);
    free(x); free(rgb);
}

static void late_parity(void)
{
    enum { W = 320, H = 240, RATE = 12000 };
    uint8_t *rgb = malloc(W*H*3);
    uint16_t reference[40*W];
    EncOpts o;
    int16_t *x;
    long n, at;
    int i, pass, w, h, changed = 0;
    assert(rgb);
    for (i = 0; i < W*H; i++) {
        rgb[3*i] = 220; rgb[3*i+1] = 70; rgb[3*i+2] = 30;
    }
    enc_default_opts(&o); o.rate = RATE;
    x = enc_encode(rgb, &o, &n); assert(x);
    for (pass = 0; pass < 2; pass++) {
        SstvRx *s;
        SstvStatus st;
        const uint16_t *img;
        if (pass) {
            /* Wrong early separators; the majority recovers after the audio ring has expired. */
            for (i = 0; i < 60; i++) {
                long a = (long)((o.lead_s + .910 + i*.150 + .100)*RATE + .5);
                long b = a + 54;
                double f = (i & 1) ? 1500 : 2300;
                for (at = a; at < b; at++) x[at] = (int16_t)(o.amplitude*sin(6.283185307179586*f*(at-a)/RATE));
            }
        }
        assert(sstv_pool_open()); s = sstv_new(RATE); assert(s && sstv_set_weak(s, 1));
        for (at = 0; at < n; at += 1024) sstv_feed(s, x+at, (int)(n-at < 1024 ? n-at : 1024), 1);
        sstv_status(s, &st); assert(st.end_kind == SSTV_END_COMPLETE);
        img = sstv_image(s, &w, &h); assert(img && w == W && h == H);
        if (!pass) memcpy(reference, img, sizeof reference);
        else for (i = 0; i < 40*W; i++) if (i % W >= 32 && i % W < W-32 && img[i] != reference[i]) changed++;
        sstv_free(s); sstv_pool_close(); assert(heap_blocks == 0);
    }
    assert(changed < 40*W/100);
    free(x); free(rgb);
    puts("PASS: late Robot36 parity recovery recolors expired rows");
}

int main(void)
{
    memory();
    late_parity();
    image(8, 12000, 0, 8, 0, 0);
    image(8, 44100, 0, 10, 120, .6);
    image(8, 48000, 1, 12, 0, 0);
    image(40, 12000, 0, 12, -120, 0);
    image(56, 44100, 0, 15, 30, 0);
    image(93, 12000, 0, 12, 40, .5);
    image(94, 44100, 0, 15, 0, 0);
    puts("PASS: 7 weak-signal cases, bounded pool and restart cleanup");
    return 0;
}
