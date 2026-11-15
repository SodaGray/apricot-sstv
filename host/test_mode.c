/* SPDX-License-Identifier: GPL-3.0-only */
#include "../sstv_dsp.h"
#include "enc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void decode(int mode, int vis, int selected, int weak, int complete)
{
    EncOpts o;
    SstvRx *s;
    SstvStatus st;
    uint8_t *rgb;
    int16_t *x;
    int w, h, i;
    long n, at;
    assert(enc_mode_size(mode, &w, &h));
    rgb = malloc((size_t)w*h*3); assert(rgb);
    for (i = 0; i < w*h; i++) { rgb[3*i] = 210; rgb[3*i+1] = 70; rgb[3*i+2] = 30; }
    enc_default_opts(&o); o.mode = mode; o.vis_code = vis; o.rate = 12000;
    if (!complete) o.lines = 20;
    x = enc_encode(rgb, &o, &n); assert(x);
    s = sstv_new(o.rate); assert(s);
    assert(!sstv_set_mode(s, 123));
    assert(sstv_set_mode(s, selected));
    assert(sstv_set_weak(s, weak));
    for (at = 0; at < n; at += 1024) sstv_feed(s, x+at, (int)(n-at < 1024 ? n-at : 1024), 1);
    assert(!sstv_set_mode(s, -1));
    sstv_status(s, &st);
    if (complete) {
        assert(st.mode && !strcmp(st.mode, enc_mode_name(mode)));
        assert(st.end_kind == SSTV_END_COMPLETE && st.line == h && st.images == 1);
        assert(st.start_kind == (vis == mode ? SSTV_START_VIS : SSTV_START_SYNC));
    } else assert(st.start_kind == SSTV_START_NONE && st.images == 0);
    sstv_free(s); free(x); free(rgb);
}

int main(void)
{
    static const int modes[] = {8, 12, 44, 40, 60, 56, 76, 93, 99, 95, 98, 96, 97, 94};
    int i;
    for (i = 0; i < 14; i++) decode(modes[i], -1, modes[i], 0, 1);
    decode(8, 8, 8, 0, 1);
    decode(8, 40, 8, 1, 1);
    decode(40, 40, 8, 0, 0);
    decode(40, -1, 8, 1, 0);
    decode(8, 8, -1, 0, 1);
    puts("PASS: 14 manual modes without VIS, matching/mismatched VIS, mode isolation, Auto and setter guards");
    return 0;
}
