/* SPDX-License-Identifier: GPL-3.0-only */
/* sstv_decode.c -- decode SSTV from a WAV file with the exact decoder
 * the iPod runs (sstv_dsp.c). Writes every image it finishes, plus the one in
 * progress at the end of the file.
 *
 *   sstv_decode recording.wav [out_prefix] [--no-autosync] [--ppm N] [--rate HZ]
 *
 * --rate overrides the WAV header: the iPod's SSTVCAP.WAV always says 44100;
 * if the app showed a different capture rate, decode with that.
 *
 * Modes: Robot 36/72, Martin M1/M2, Scottie S1/S2/DX, PD50/90/120/160/180/240/290.
 * Images: <out_prefix>-1.ppm, -2.ppm ... (default prefix "sstv"), full size.
 * Prints a line per event: VIS heard, image started/finished, fit result. */
#include "../sstv_dsp.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *start_name(int k) { return k == SSTV_START_VIS ? "VIS" : k == SSTV_START_SYNC ? "sync run (no VIS)" : "-"; }
static const char *end_name(int k)
{
    switch (k) {
    case SSTV_END_COMPLETE: return "complete";
    case SSTV_END_LOST:     return "signal lost";
    case SSTV_END_ABORT:    return "aborted";
    case SSTV_END_NEWVIS:   return "new VIS";
    case SSTV_END_NOMEM:    return "out of memory";
    default:                return "-";
    }
}

int main(int argc, char **argv)
{
    const char *in = 0, *prefix = "sstv";
    int i, ch, rate, autosync = 1, nimg = 0, have_ppm = 0, ppm = 0, force_rate = 0;
    long frames, f;
    int16_t *x;
    SstvRx *s;
    SstvStatus S, P;
    char path[1024];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-autosync")) autosync = 0;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) { ppm = atoi(argv[++i]); have_ppm = 1; }
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) force_rate = atoi(argv[++i]);
        else if (!in) in = argv[i];
        else prefix = argv[i];
    }
    if (!in) { fprintf(stderr, "usage: %s in.wav [out_prefix] [--no-autosync] [--ppm N] [--rate HZ]\n", argv[0]); return 2; }
    x = wav_read(in, &frames, &ch, &rate);
    if (!x) { fprintf(stderr, "%s: can't read (PCM 16/24/32-bit or float WAV)\n", in); return 1; }
    if (force_rate > 0) { printf("(header says %d Hz; decoding as %d Hz)\n", rate, force_rate); rate = force_rate; }
    printf("%s: %d Hz, %d ch, %.1f s (decoding channel 1)\n", in, rate, ch, (double)frames / rate);
    s = sstv_new(rate);
    if (!s) { fprintf(stderr, "decoder: rate %d unsupported or out of memory\n", rate); return 1; }
    sstv_set_autosync(s, autosync);
    if (have_ppm) sstv_set_ppm_prior(s, ppm);
    sstv_status(s, &P);
    for (f = 0; f < frames; f += 1024) {
        int m = (int)(frames - f < 1024 ? frames - f : 1024);
        sstv_feed(s, x + f * ch, m, ch);
        sstv_status(s, &S);
        if (S.last_vis != P.last_vis || (S.revision != P.revision && S.last_vis >= 0 && P.last_vis < 0)) {
            const char *nm = sstv_vis_name(S.last_vis);
            printf("%7.2f s  VIS %d (%s)%s\n", (double)f / rate, S.last_vis, nm ? nm : "unknown",
                   S.last_vis_ok ? "" : " - not decoded by this build");
        }
        if (S.state == SSTV_RX && (P.state != SSTV_RX || S.line < P.line))
            printf("%7.2f s  %s started (%s), tuning offset %+d Hz\n", (double)f / rate, S.mode, start_name(S.start_kind), S.offset_hz);
        if (S.state != SSTV_RX && P.state == SSTV_RX) {
            snprintf(path, sizeof path, "%s-%d.ppm", prefix, ++nimg);
            { int iw, ih; const uint16_t *im = sstv_image(s, &iw, &ih); ppm_write_565(path, im, iw, ih); }
            printf("%7.2f s  image ended (%s): %s, %d/%d rows, syncs %d/%d, clock %+d ppm%s -> %s\n",
                   (double)f / rate, end_name(S.end_kind), S.mode, S.line, S.height, S.sync_good, S.sync_seen, S.ppm,
                   S.ppm_valid ? "" : " (prior)", path);
        }
        P = S;
    }
    if (S.state == SSTV_RX) {
        snprintf(path, sizeof path, "%s-%d.ppm", prefix, ++nimg);
        { int iw, ih; const uint16_t *im = sstv_image(s, &iw, &ih); ppm_write_565(path, im, iw, ih); }
        printf("  end of file mid-image (%s, %d/%d rows) -> %s\n", S.mode, S.line, S.height, path);
    }
    if (!nimg) printf("no image found\n");
    sstv_free(s);
    free(x);
    return 0;
}
