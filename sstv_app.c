/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * sstv_app.c -- Apricot SSTV: a NanoApps RAW-surface app that receives
 * slow-scan TV on the iPod nano 7th generation (RetailOS 1.1.2).
 * Modes: Robot 36/72, Martin M1/M2, Scottie S1/S2/DX, PD50/90/120/160/180/240/290.
 *
 *   capture DMA (44100, L == R) --nano_cap--> sstv_dsp (FPU lib) --> picture
 *
 * Landscape UI drawn into the portrait framebuffer: a 320x240 picture area
 * (larger pictures are scaled to fit) with a 112 px status/control strip.
 *
 * Execution rules:
 *   - all float work (the decoder) runs in hb_raw_frame, never hb_raw_init;
 *   - capture uses the system's wired capture device (the one Voice Memos
 *     uses). It is normally set up at boot; if not, LISTEN says so, and
 *     rebooting and opening Voice Memos once sets it up. The app installs no
 *     firmware callback and starts no thread; it only calls the device's
 *     start method and reads the DMA buffers.
 *
 * Memory: code + .bss is kept small (~465 KB: picture view, strip shadow,
 * fonts). A large .bss (~1 MB) crashed the system part-way through long
 * pictures, so the decoder takes one fixed 540 KB block from the OS heap at
 * LISTEN (only with 1 MB to spare) and gives it back at STOP; see
 * sstv_support.c. REC borrows 1 MB the same way. The strip shows the OS
 * heap's free space.
 *
 * Frame budget: nano_cap reads the DMA's live position, so frame gaps up to
 * ~45 ms lose nothing (UI frames measured 16-31 ms on the device). Longer
 * stalls are counted (LOST) and the decoder's timeline is kept exact.
 */
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_raw_surface.h"
#include "sstv_dsp.h"
#include "nano_cap.h"
#include "sstv_font5x7.h"
#include "gsf.h"
#include "font_io.h"
#include "version.h"
#include <stddef.h>
#include <stdint.h>

/* OS heap headroom: REC (1 MB) only with this much left over, BMP save only
 * above SAVE_FLOOR (the file system takes heap while writing). The OS
 * allocator panics rather than failing when it runs out. */
#define REC_FLOOR   (1024u * 1024u)
#define SAVE_FLOOR  (512u * 1024u)
/* breadcrumbs while listening (./start trace shows the last 48 after a crash):
 *   TICK  a = seconds since the last touch << 16 | slowest frame interval, ms
 *         b = frames in the window << 16 | most work in one frame, ms
 *   DEC   a = decoder state << 28 | last VIS (0xFF none) << 16 | rows decoded
 *         b = images << 16 | sync % (good of seen)
 *   HEAP  a = OS heap free, b = lowest seen (every 3rd TICK) */
#define TICK_US      10000000u
#define HEAP_POLL_US  1000000u          /* hb_os_heap_free at most once a second */

int   sstv_pool_open(void);
void  sstv_pool_close(void);
#define POOL_FLOOR  (1024u * 1024u)    /* OS heap to leave free after taking the pool */

#define TOUCH_DOWN(t) ((t) != 0 && (t)->down)

/* the picture area (landscape): pictures larger than this are scaled to fit */
#define SSTV_W 320
#define SSTV_H 240

#define C_BG      0xFF101218u
#define C_PANEL   0xFF181C24u
#define C_FG      0xFFE8E8F0u
#define C_DIM     0xFF8088A0u
#define C_OK      0xFF3AD06Au
#define C_WARN    0xFFFFB02Eu
#define C_ERR     0xFFE04040u
#define C_BTN     0xFF242A36u
#define C_BTN_HI  0xFF3460C0u
#define C_BTN_ON  0xFF2E6B3Eu

/* ------------------------------------------------------------ landscape */
static uint32_t *g_fb;
static int g_w, g_h;            /* portrait framebuffer (240 x 432) */
static int g_lw, g_lh;          /* landscape view (432 x 240) */
static int g_rot;               /* which way the iPod is turned */
#define PANEL_X 322

/* ---- what reaches the framebuffer, and how often ----
 * On the device the framebuffer appears to be uncached memory the display
 * DMA reads, so every store is slow: rewriting all ~250k pixels each frame
 * took up to 131 ms, longer than the 46 ms capture ring, and lost audio. So:
 *   - the status strip (landscape x >= 320, 112 x 240) is drawn into a cached
 *     shadow, and only 16-px segments that changed are copied out;
 *   - picture rows are copied out only when their contents changed;
 *   - nothing else is touched. A sentinel pixel checks that the framebuffer
 *     keeps its contents between frames; if it doesn't, every frame is a
 *     full redraw (as before), and the strip shows FULL. */
#define STRIP_X  SSTV_W                 /* landscape x where the shadowed strip starts */
#define STRIP_W  112
#define SEG_W    16
#define NSEG     (STRIP_W / SEG_W)
#define C_SENT   (C_PANEL ^ 0x00010001u)  /* looks like the panel, but recognisable */
static uint32_t g_pb[SSTV_H * STRIP_W];          /* strip shadow (cached) */
/* What each framebuffer currently shows, as hashes. Two slots, in case the
 * runtime double-buffers (hb_raw_fb alternating): each buffer is brought up
 * to date from its own last contents. */
typedef struct {
    uint32_t *fb;
    int       rot, full, pic_src;
    unsigned  pic_rev;
    uint32_t  row[SSTV_H];
    uint32_t  seg[SSTV_H][NSEG];
} FbSlot;
static FbSlot   g_slot[2];
static FbSlot  *g_cur;                            /* slot of this frame's framebuffer */
static int      g_slot_next;
static int      g_full = 1;                       /* this frame rewrites everything */
static int      g_persist = 1;                    /* framebuffer keeps its contents? */
static int      g_sent_miss;

static inline uint32_t *fb_at(int lx, int ly)
{
    int px, py;
    if (!g_rot) { px = g_w - 1 - ly; py = lx; } else { px = ly; py = g_h - 1 - lx; }
    return g_fb + py * g_w + px;
}
/* all UI drawing goes to the strip shadow, or (g_to_view, for the splash)
 * into the picture view as RGB565 */
static int       g_to_view;
static uint16_t *g_view_px;                       /* = g_view (declared further down) */
static inline void lput(int lx, int ly, uint32_t c)
{
    if (g_to_view) {
        if ((unsigned)lx < (unsigned)SSTV_W && (unsigned)ly < (unsigned)SSTV_H)
            g_view_px[ly * SSTV_W + lx] = (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
        return;
    }
    lx -= STRIP_X;
    if ((unsigned)lx >= (unsigned)STRIP_W || (unsigned)ly >= (unsigned)SSTV_H) return;
    g_pb[ly * STRIP_W + lx] = c;
}
static void lfill(int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) lput(x + i, y + j, c);
}
static void lframe(int x, int y, int w, int h, uint32_t c)
{
    lfill(x, y, w, 1, c); lfill(x, y + h - 1, w, 1, c);
    lfill(x, y, 1, h, c); lfill(x + w - 1, y, 1, h, c);
}
static void lchar(int x, int y, int sc, char ch, uint32_t c)
{
    const uint8_t *g = sf_glyph(ch);
    int r, col;
    for (r = 0; r < SF_H; r++)
        for (col = 0; col < SF_W; col++)
            if (g[r] & (1u << (4 - col))) lfill(x + col * sc, y + r * sc, sc, sc, c);
}

/* ---- text: GSF1 bitmap fonts from /Apps/Data/Fonts, 5x7 fallback ----
 * Two roles, each with its own candidate files (font_io.h), read by gsf.c.
 * A character the font lacks is drawn in the built-in 5x7, sitting on the
 * font's baseline; a role with no font at all is drawn entirely in 5x7
 * (scale 1 for UI, 2 for the title). */
enum { FONT_UI = 0, FONT_TITLE = 1 };
static Gsf  g_font[2];
static int  g_font_ok[2], g_fonts_tried;
static char g_font_name[2][24];

static void glyph_draw(const Gsf *f, const GsfGlyph *g, int x, int top, uint32_t c)
{
    int gy = top + (int)f->info.line_height - f->info.base_line - g->box_h - g->ofs_y;
    int gx = x + g->ofs_x, n = g->box_w * g->box_h, i;
    for (i = 0; i < n; i++)
        if (g->bits[i >> 3] & (0x80u >> (i & 7))) lput(gx + i % g->box_w, gy + i / g->box_w, c);
}

static int ui_line_h(int role)
{
    if (g_font_ok[role]) return (int)g_font[role].info.line_height;
    return role == FONT_TITLE ? 16 : 10;
}
/* draw (or just measure, draw = 0) text with its line box's top-left at
 * (x, y), stopping before max_w px of advance (max_w <= 0: no limit) */
static int run_text(int x, int y, int role, const char *s, uint32_t c, int max_w, int draw)
{
    int pen = 0;
    if (g_font_ok[role]) {
        const Gsf *f = &g_font[role];
        int base = y + (int)f->info.line_height - f->info.base_line;
        for (; *s; s++) {
            GsfGlyph g;
            int adv;
            if (gsf_glyph(f, (unsigned char)*s, &g)) {
                adv = g.advance;
                if (max_w > 0 && pen + adv > max_w) break;
                if (draw) glyph_draw(f, &g, x + pen, y, c);
            } else {                                /* missing: built-in, on the baseline */
                adv = SF_W + 1;
                if (max_w > 0 && pen + adv - 1 > max_w) break;
                if (draw) lchar(x + pen, base - SF_H, 1, *s, c);
            }
            pen += adv;
        }
        return pen;
    } else {
        int sc = role == FONT_TITLE ? 2 : 1, adv = (SF_W + 1) * sc;
        for (; *s; s++) {
            if (max_w > 0 && pen + adv - sc > max_w) break;
            if (draw) lchar(x + pen, y + (role == FONT_TITLE ? 0 : 1), sc, *s, c);
            pen += adv;
        }
        return pen ? pen - sc : 0;                  /* no gap after the last glyph */
    }
}
static void ui_text(int x, int y, int role, const char *s, uint32_t c, int max_w) { run_text(x, y, role, s, c, max_w, 1); }
static int  ui_text_w(int role, const char *s) { return run_text(0, 0, role, s, 0, 0, 0); }

static void fonts_load(void)
{
    static const char *const ui[] = SSTV_FONTS_UI, *const title[] = SSTV_FONTS_TITLE;
    const char *const *cand[2] = { ui, title };
    int r, i, k;
    g_fonts_tried = 1;
    for (r = 0; r < 2; r++) {
        g_font_ok[r] = 0; g_font_name[r][0] = 0;
        for (i = 0; cand[r][i] && !g_font_ok[r]; i++) {
            uint32_t n;
            const uint8_t *d = font_file_load(r, cand[r][i], &n);
            if (d && gsf_open(&g_font[r], d, n) == 0 && g_font[r].info.line_height <= (r ? 28u : 16u)) {
                g_font_ok[r] = 1;
                for (k = 0; k < 23 && cand[r][i][k] && cand[r][i][k] != '.'; k++) g_font_name[r][k] = cand[r][i][k];
                g_font_name[r][k] = 0;
            }
        }
        hb_trace_log("FONT", (uint32_t)r, (uint32_t)g_font_ok[r]);
    }
}

/* landscape coordinates of a touch */
static void touch_l(const hb_spoint_t *t, int *lx, int *ly)
{
    int px = t->x, py = t->y;
    if (!g_rot) { *lx = py; *ly = g_w - 1 - px; } else { *lx = g_h - 1 - py; *ly = px; }
}

static uint32_t pic_px(int i);

/* ------------------------------------------------------------ strings */
static char *put_u(char *o, uint32_t v)
{
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) *o++ = t[--k];
    *o = 0;
    return o;
}
static char *put_i(char *o, int v)
{
    *o++ = v < 0 ? '-' : '+';
    return put_u(o, (uint32_t)(v < 0 ? -v : v));
}
static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *put_k(char *o, uint32_t bytes) { o = put_u(o, (bytes + 512u) / 1024u); return put_s(o, "K"); }

/* ------------------------------------------------------------ state */
static SstvRx  *g_rx;                  /* in the static pool; kept after STOP for SAVE */
/* What the picture area shows, RGB565: the decoder's picture scaled to fit
 * 320x240 (area average), kept up to date from its changed rows. It's also
 * what stays on screen after STOP (it lives in the app arena, freed on exit). */
static uint16_t g_view[SSTV_W * SSTV_H];
static int      g_have_still;               /* g_view holds a picture */
static unsigned g_view_rev;                 /* bumps when g_view changes */
static int      g_vw, g_vh, g_vx, g_vy;     /* scaled size and offset within the area */
static int      g_src_w, g_src_h;           /* the picture g_view was made from */
static SstvStatus g_last_st;           /* status at STOP, for the strip */
static int      g_ppm_prior, g_ppm_known;   /* clock learned in earlier sessions */
static unsigned long g_images_base;    /* images completed by earlier decoders */
static uint32_t g_heap, g_heap_min = 0xFFFFFFFFu, g_heap_t;  /* OS heap free: now, lowest seen */
static uint32_t g_touch_t, g_tick_t, g_tick_n, g_tick_frames, g_tick_fmax, g_tick_wmax;
static NanoCap  g_cap;
static int      g_listening;
static int      g_autosave = 1;
static unsigned long g_images_seen, g_saved;
static int      prev_down;
#define MSG_MAX 31              /* (drawn clipped to the strip width) */
static char     g_msg[2][MSG_MAX + 1];
static uint32_t g_msg_col;

static void say(const char *a, const char *b, uint32_t col)
{
    int i;
    for (i = 0; i < MSG_MAX && a && a[i]; i++) g_msg[0][i] = a[i];
    g_msg[0][i] = 0;
    for (i = 0; i < MSG_MAX && b && b[i]; i++) g_msg[1][i] = b[i];
    g_msg[1][i] = 0;
    g_msg_col = col;
}

static uint32_t now_us(void) { return (uint32_t)hb_time_uptime_us(); }

/* capture -> decoder */
/* ---- diagnostics: measured capture rate, raw recording ----
 * RATE = frames the DMA delivered (+ counted as lost) per second of CPU
 * clock, over a window of a few seconds. It should read ~44100; anything else
 * means the codec isn't running at the rate everything here assumes.
 * REC records 12 s of exactly what the decoder is fed (gaps as silence) and
 * writes /SSTVCAP.WAV, for sstv_decode / inspection on the Mac. */
#define REC_FRAMES (12u * NCAP_RATE)
static int16_t *g_rec;
static uint32_t g_rec_n;
static uint32_t g_rate, g_rate_t0, g_rate_f0;
static int      g_rate_started, g_rate_checked;
static uint32_t g_dec_rate = NCAP_RATE;     /* rate the decoder was built for */
/* frame timing: interval between hb_raw_frame calls and our time inside one */
static uint32_t g_fr_last, g_fr_avg, g_fr_max, g_wk_avg, g_wk_max;

static void rec_put(const int16_t *x, int n)
{
    int i;
    if (!g_rec) return;
    for (i = 0; i < n && g_rec_n < REC_FRAMES; i++) g_rec[g_rec_n++] = x ? x[i] : 0;
}
static void rec_finish(void)
{
    uint8_t h[44];
    uint32_t bytes = g_rec_n * 2u, v, off;
    int i, ok;
    if (!g_rec) return;
    for (i = 0; i < 44; i++) h[i] = 0;
    h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
    for (i = 0, v = 36u + bytes; i < 4; i++, v >>= 8) h[4 + i] = (uint8_t)v;
    h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
    h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
    h[16] = 16; h[20] = 1; h[22] = 1;                            /* PCM, mono */
    for (i = 0, v = NCAP_RATE; i < 4; i++, v >>= 8) h[24 + i] = (uint8_t)v;
    for (i = 0, v = NCAP_RATE * 2u; i < 4; i++, v >>= 8) h[28 + i] = (uint8_t)v;
    h[32] = 2; h[34] = 16;
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    for (i = 0, v = bytes; i < 4; i++, v >>= 8) h[40 + i] = (uint8_t)v;
    hb_trace_log("REC", g_rec_n, 0);
    ok = hb_fs_stream_open("/SSTVCAP.WAV") && hb_fs_stream_write(h, 44u);
    for (off = 0; ok && off < bytes; off += 32768u) {            /* write in 32 KB pieces */
        uint32_t n = bytes - off < 32768u ? bytes - off : 32768u;
        ok = hb_fs_stream_write((const uint8_t *)g_rec + off, n);
    }
    ok = hb_fs_stream_close() && ok;
    hb_os_free(g_rec); g_rec = 0;
    if (ok) say("Rec saved", "SSTVCAP.WAV", C_OK); else say("Rec write failed", "", C_ERR);
}
static char *put_k(char *o, uint32_t bytes);
static void rec_start(void)
{
    uint32_t need = REC_FRAMES * (uint32_t)sizeof *g_rec, fr;
    char b[MSG_MAX + 1], *p;
    if (g_rec || !g_listening) return;
    fr = hb_os_heap_free();
    if (fr < need + REC_FLOOR) {                    /* the OS allocator panics when it runs out */
        p = put_s(b, "need "); p = put_k(p, need + REC_FLOOR); p = put_s(p, ", "); put_k(p, fr);
        say("Rec: low memory", b, C_WARN);
        return;
    }
    g_rec = (int16_t *)hb_os_alloc(need);
    g_rec_n = 0;
    if (!g_rec) say("Rec: no memory", "", C_ERR);
    else say("Recording 12 s", "", C_WARN);
}

/* ------------------------------------------------------------ level meter
 * dBFS, 0 dB = 32768 (the s16 limit). Integer only (soft-float side).
 * on_audio accumulates this frame's peak and sum of squares; vu_update turns
 * them into the bar once per frame:
 *   bar  = RMS, smoothed over ~0.2 s (VU-like)
 *   tick = peak, held 1.5 s, then falling 20 dB/s
 *   CLIP = a sample at +-32767/-32768 in the last 2 s */
#define VU_MIN_DB  48                    /* bar spans -48 .. 0 dBFS */
static const uint16_t k_vu_amp[61] = {   /* 32768 * 10^(-d/20), d = 0..60 */
    32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362, 9235, 8231,
    7336, 6538, 5827, 5193, 4629, 4125, 3677, 3277, 2920, 2603, 2320, 2068, 1843, 1642, 1464,
    1305, 1163, 1036, 924, 823, 734, 654, 583, 519, 463, 413, 368, 328, 292, 260, 232, 207,
    184, 164, 146, 130, 116, 104, 92, 82, 73, 65, 58, 52, 46, 41, 37, 33 };
static uint64_t g_vu_sum;               /* this frame: sum of x^2 */
static uint32_t g_vu_n, g_vu_pk;        /* ... samples, |x| peak */
static uint32_t g_vu_ms;                /* smoothed mean square */
static int      g_vu_pk_db10 = -999;    /* held peak, tenths of dB */
static uint32_t g_vu_pk_t, g_vu_clip_t, g_vu_last_t;
static int      g_vu_rms_db = -99, g_vu_clip;

static int amp_db(uint32_t a)           /* -> whole dBFS, 0 .. -60 (or -99 for silence) */
{
    int d;
    if (a == 0u) return -99;
    for (d = 0; d < 61; d++) if (a >= k_vu_amp[d]) return -d;
    return -99;
}
static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}
static void vu_put(const int16_t *x, int n)
{
    int i;
    uint32_t pk = g_vu_pk;
    uint64_t sum = g_vu_sum;
    for (i = 0; i < n; i++) {
        int32_t v = x[i];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > pk) pk = a;
        sum += (uint32_t)(v * v);
    }
    g_vu_pk = pk; g_vu_sum = sum; g_vu_n += (uint32_t)n;
}
static void vu_update(uint32_t now)
{
    uint32_t dt = g_vu_last_t ? now - g_vu_last_t : 0u;
    g_vu_last_t = now;
    if (!g_listening) {                              /* idle: let it fall to nothing */
        g_vu_ms = 0; g_vu_rms_db = -99; g_vu_pk_db10 = -999; g_vu_clip = 0;
        g_vu_sum = 0; g_vu_n = 0; g_vu_pk = 0;
        return;
    }
    if (g_vu_n) {
        uint32_t ms = (uint32_t)(g_vu_sum / g_vu_n);  /* <= 2^30 */
        int pdb = amp_db(g_vu_pk) * 10;
        g_vu_ms = g_vu_ms - g_vu_ms / 8u + ms / 8u;  /* ~0.2 s at 25 ms frames */
        if (g_vu_pk >= 32767u) g_vu_clip_t = now, g_vu_clip = 1;
        if (pdb >= g_vu_pk_db10) { g_vu_pk_db10 = pdb; g_vu_pk_t = now; }
    }
    if (g_vu_clip && now - g_vu_clip_t > 2000000u) g_vu_clip = 0;
    if (now - g_vu_pk_t > 1500000u && g_vu_pk_db10 > -999) {
        g_vu_pk_db10 -= (int)(dt / 5000u);           /* 20 dB/s = 0.2 dB per 10 ms */
        if (g_vu_pk_db10 < -999) g_vu_pk_db10 = -999;
    }
    g_vu_rms_db = amp_db(isqrt32(g_vu_ms));
    g_vu_sum = 0; g_vu_n = 0; g_vu_pk = 0;
}
/* the bar: -48 .. 0 dBFS, green to -12, amber to -3, red above; peak tick;
 * the last 5 px light red on CLIP */
static void vu_bar(int x, int y, int w)
{
    static const int ticks[6] = { 40, 30, 20, 12, 6, 3 };
    int h = 12, iw = w - 8, i, lit;                  /* 6 px clip lamp at the right */
    lfill(x, y, w, h, 0xFF0C0E12u);
    lframe(x, y, iw + 2, h, C_DIM);
    lit = g_vu_rms_db <= -VU_MIN_DB ? 0 : (VU_MIN_DB + g_vu_rms_db) * iw / VU_MIN_DB;
    for (i = 0; i < lit; i++) {
        int db = -VU_MIN_DB + (i * VU_MIN_DB + VU_MIN_DB - 1) / iw;   /* dB at this column */
        lfill(x + 1 + i, y + 2, 1, h - 4, db > -3 ? C_ERR : db > -12 ? C_WARN : C_OK);
    }
    for (i = 0; i < 6; i++) {                        /* scale ticks along the bottom */
        int tx = x + 1 + (VU_MIN_DB - ticks[i]) * iw / VU_MIN_DB;
        lfill(tx, y + h - 3, 1, 2, 0xFF505868u);
    }
    if (g_vu_pk_db10 > -VU_MIN_DB * 10) {            /* peak tick */
        int px = x + 1 + (VU_MIN_DB * 10 + g_vu_pk_db10) * iw / (VU_MIN_DB * 10);
        if (px > x + iw) px = x + iw;
        lfill(px - 1, y + 1, 2, h - 2, g_vu_pk_db10 > -30 ? C_FG : 0xFFB0B8C8u);
    }
    lfill(x + w - 6, y, 6, h, g_vu_clip ? C_ERR : 0xFF301418u);   /* clip lamp */
}

static void on_audio(void *u, const int16_t *mono, int n)
{
    (void)u;
    rec_put(mono, n);
    vu_put(mono, n);
    if (g_rx) sstv_feed(g_rx, mono, n, 1);
}
static void on_gap(void *u, int n)
{
    (void)u;
    rec_put(0, n);
    if (!g_rx) return;
    /* keep the picture's timing exact (cheap: no filtering of silence). Only
     * a very long stall drops the picture, and even then the decoder resumes
     * it if its syncs come back on the same line grid. */
    if (n > 30 * (int)NCAP_RATE) sstv_abort(g_rx);
    else sstv_gap(g_rx, n);
}

static void view_update(void);

/* runs in hb_raw_frame (FPU live): sstv_new does float init */
static void listen_start(void)
{
    int r;
    if (g_rx) {                                     /* last session's decoder: start afresh */
        SstvStatus o;
        sstv_status(g_rx, &o);
        g_images_base += o.images;
        sstv_free(g_rx); g_rx = 0;
    }
    if (!sstv_pool_open()) {
        say("Decoder: no memory", "(OS heap)", C_ERR); return;
    }
    if (hb_os_heap_free() < POOL_FLOOR) {                   /* the OS needs room too */
        char b[MSG_MAX + 1];
        put_k(put_s(b, "heap left "), hb_os_heap_free());
        sstv_pool_close();
        say("Low memory: not started", b, C_WARN);
        return;
    }
    g_rx = sstv_new((int)g_dec_rate);
    if (!g_rx) { sstv_pool_close(); say("Decoder:", "no memory (pool)", C_ERR); return; }
    if (g_ppm_known) sstv_set_ppm_prior(g_rx, g_ppm_prior);
    g_images_seen = 0;
    ncap_init(&g_cap, ncap_nano_bus());
    hb_trace_log("SSTVON", 0, 0);
    r = ncap_arm(&g_cap, now_us());
    if (r == NCAP_E_NOTPRIMED) {
        sstv_free(g_rx); g_rx = 0; sstv_pool_close();
        say("Not ready: reboot", "then Voice Memos", C_WARN);
        return;
    }
    g_listening = 1;
    g_src_w = g_src_h = 0;              /* last picture stays up until the first new one starts */
    g_rate = 0; g_rate_started = 0; g_rate_checked = 0;
    g_fr_max = g_wk_max = 0;
    say("Listening", g_cap.mode == NCAP_MODE_DMAPOS ? "DMA position OK" : "Cursor mode", C_OK);
}
static void listen_stop(void)
{
    hb_trace_log("SSTVOFF", g_cap.frames_out, g_cap.frames_lost);
    if (g_rec) rec_finish();
    ncap_disarm(&g_cap);
    g_listening = 0;
    if (g_rx) {
        sstv_abort(g_rx);
        view_update();                                /* the view keeps the (scaled) picture */
        sstv_status(g_rx, &g_last_st);
        if (g_last_st.ppm_valid) { g_ppm_prior = g_last_st.ppm; g_ppm_known = 1; }
        g_images_base += g_last_st.images;
        sstv_free(g_rx); g_rx = 0;                    /* SAVE after STOP writes the view */
    }
    sstv_pool_close();
    say("Stopped", "", C_DIM);
}

static uint32_t x565(uint16_t v)
{
    return ((uint32_t)(v & 0xF800u) << 8 | (uint32_t)(v & 0xE000u) << 3) |
           ((uint32_t)(v & 0x07E0u) << 5 | (uint32_t)(v & 0x0600u) >> 1) |
           ((uint32_t)(v & 0x001Fu) << 3 | (uint32_t)(v & 0x001Cu) >> 2);
}
/* the shown picture as 0x00RRGGBB */
static int g_splash_on;                     /* g_view holds the version splash */
static uint32_t pic_px(int i) { return (g_have_still || g_splash_on) ? x565(g_view[i]) : 0; }

/* Version splash in the picture area while nothing has been received:
 * APP_SPLASH in the title font, APP_WEBSITE in white below it, a hint under
 * that, centred as a block. */
static void splash_draw(void)
{
    int i, w, cx = SSTV_W / 2, th = ui_line_h(FONT_TITLE), lh = ui_line_h(FONT_UI);
    int y = (SSTV_H - (th + 6 + lh + 18 + lh)) / 2;
    for (i = 0; i < SSTV_W * SSTV_H; i++) g_view[i] = 0x0841u;           /* near-black */
    g_view_px = g_view; g_to_view = 1;
    w = ui_text_w(FONT_TITLE, APP_SPLASH);
    ui_text(cx - w / 2, y, FONT_TITLE, APP_SPLASH, C_FG, SSTV_W - 8);
    y += th + 6;
    w = ui_text_w(FONT_UI, APP_WEBSITE);
    ui_text(cx - w / 2, y, FONT_UI, APP_WEBSITE, 0xFFFFFFFFu, SSTV_W - 8);
    y += lh + 18;
    w = ui_text_w(FONT_UI, "Tap Listen to start");
    ui_text(cx - w / 2, y, FONT_UI, "Tap Listen to start", C_DIM, SSTV_W - 8);
    g_to_view = 0;
}

/* Bring view rows up to date from the decoder's changed rows: each view
 * pixel averages the source pixels it covers. */
static void view_rows(const uint16_t *src, int w, int h, int r0, int r1)
{
    int r, x;
    for (r = r0; r <= r1; r++) {
        int sy0 = (r - g_vy) * h / g_vh, sy1 = (r - g_vy + 1) * h / g_vh;
        uint16_t *out = g_view + r * SSTV_W + g_vx;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < g_vw; x++) {
            int sx0 = x * w / g_vw, sx1 = (x + 1) * w / g_vw, xx, yy;
            uint32_t R = 0, G = 0, B = 0, n = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (yy = sy0; yy < sy1; yy++)
                for (xx = sx0; xx < sx1; xx++) {
                    uint16_t v = src[yy * w + xx];
                    R += v >> 11; G += (v >> 5) & 63u; B += v & 31u; n++;
                }
            out[x] = (uint16_t)(((R + n / 2) / n) << 11 | ((G + n / 2) / n) << 5 | ((B + n / 2) / n));
        }
    }
    g_view_rev++;
}
static void view_update(void)
{
    int w, h, y0, y1, r0, r1;
    const uint16_t *src;
    if (!g_rx) return;
    src = sstv_image(g_rx, &w, &h);
    if (!src || !w || !h) return;
    if (w != g_src_w || h != g_src_h || !g_have_still) {   /* new picture size: re-lay out */
        int i;
        g_src_w = w; g_src_h = h;
        if (w * SSTV_H <= h * SSTV_W) { g_vh = SSTV_H; g_vw = w * SSTV_H / h; }
        else { g_vw = SSTV_W; g_vh = h * SSTV_W / w; }
        g_vx = (SSTV_W - g_vw) / 2; g_vy = (SSTV_H - g_vh) / 2;
        for (i = 0; i < SSTV_W * SSTV_H; i++) g_view[i] = 0;
        g_have_still = 1; g_view_rev++;
        sstv_dirty(g_rx, &y0, &y1);
        view_rows(src, w, h, g_vy, g_vy + g_vh - 1);
        return;
    }
    if (!sstv_dirty(g_rx, &y0, &y1)) return;
    r0 = g_vy + y0 * g_vh / h;                         /* view rows that see source rows y0..y1 */
    r1 = g_vy + ((y1 + 1) * g_vh + h - 1) / h - 1;
    if (r1 >= g_vy + g_vh) r1 = g_vy + g_vh - 1;
    view_rows(src, w, h, r0, r1);
}

/* ------------------------------------------------------------ BMP save */
static int save_bmp(void)
{
    static uint8_t chunk[8 * SSTV_MAX_W * 3];
    uint8_t h[54];
    char path[24], *p;
    const uint16_t *src = 0;
    int W = SSTV_W, H = SSTV_H, y, x, i, k, ok;
    uint32_t v, rowb, size;
    if (g_rx) src = sstv_image(g_rx, &W, &H);                /* the stored picture */
    if (!src) { src = g_view; W = SSTV_W; H = SSTV_H; }       /* after STOP: what's shown */
    if (!g_have_still) { say("Nothing to save", "", C_DIM); return 0; }
    if (hb_os_heap_free() < SAVE_FLOOR) {                     /* writing takes OS heap */
        char b[MSG_MAX + 1];
        put_k(put_s(b, "heap "), hb_os_heap_free());
        say("Not saved: low mem", b, C_WARN);
        hb_trace_log("SAVELOW", hb_os_heap_free(), 0);
        return 0;
    }
    rowb = ((uint32_t)W * 3u + 3u) & ~3u;
    size = 54u + rowb * (uint32_t)H;
    for (i = 0; i < 54; i++) h[i] = 0;
    h[0] = 'B'; h[1] = 'M';
    for (i = 0, v = size; i < 4; i++, v >>= 8) h[2 + i] = (uint8_t)v;
    h[10] = 54; h[14] = 40;
    h[18] = (uint8_t)(W & 255); h[19] = (uint8_t)(W >> 8);
    h[22] = (uint8_t)(H & 255); h[23] = (uint8_t)(H >> 8);
    h[26] = 1; h[28] = 24;
    for (i = 0, v = rowb * (uint32_t)H; i < 4; i++, v >>= 8) h[34 + i] = (uint8_t)v;
    p = put_s(path, "/SSTV_");
    p = put_u(p, (uint32_t)hb_time_uptime_ms() / 1000u);
    put_s(p, ".BMP");
    hb_trace_log("SAVE", g_saved, (uint32_t)(W * 1000 + H));
    ok = hb_fs_stream_open(path) && hb_fs_stream_write(h, 54u);
    for (y = H - 1; ok && y >= 0; ) {                 /* BMP rows are bottom-up, BGR */
        for (k = 0; k < 8 && y >= 0; k++, y--) {
            uint8_t *d = chunk + (uint32_t)k * rowb;
            for (x = 0; x < W; x++) {
                uint32_t c = x565(src[y * W + x]);
                d[x * 3] = (uint8_t)c; d[x * 3 + 1] = (uint8_t)(c >> 8); d[x * 3 + 2] = (uint8_t)(c >> 16);
            }
            for (x = W * 3; x < (int)rowb; x++) d[x] = 0;
        }
        ok = hb_fs_stream_write(chunk, (uint32_t)k * rowb);
    }
    ok = hb_fs_stream_close() && ok;
    if (ok) { g_saved++; say("Saved", path + 1, C_OK); }
    else say("Save failed", path + 1, C_ERR);
    return ok;
}

/* ------------------------------------------------------------ widgets */
static int button(int down, int press, int tx, int ty, int x, int y, int w, int h,
                  const char *s, int role, uint32_t bg)
{
    int inside = down && tx >= x && tx < x + w && ty >= y && ty < y + h;
    char lab[24];
    const char *bar = s;
    int tw, i;
    /* "Long|Short": use the short label when the long one doesn't fit */
    while (*bar && *bar != '|') bar++;
    for (i = 0; i < 23 && s + i < bar; i++) lab[i] = s[i];
    lab[i] = 0;
    if (*bar && ui_text_w(role, lab) > w - 4) {
        for (i = 0; i < 23 && bar[1 + i]; i++) lab[i] = bar[1 + i];
        lab[i] = 0;
    }
    s = lab;
    tw = ui_text_w(role, s);
    lfill(x, y, w, h, inside ? C_BTN_HI : bg);
    lframe(x, y, w, h, C_DIM);
    ui_text(x + (tw < w - 2 ? (w - tw) / 2 : 1), y + (h - ui_line_h(role)) / 2, role, s, C_FG, w - 2);
    return press && inside;
}

/* status lines: laid out top-down at the UI font's line height; when they
 * don't all fit above the buttons (a taller GSF font), the lowest-priority
 * ones are left out */
#define MAXL 12
typedef struct { char t[40]; uint32_t c; int prio, bar; } SLine;
static SLine g_ln[MAXL];
static int   g_nl;
static char *ln_add(int prio, uint32_t c)
{
    SLine *l = &g_ln[g_nl < MAXL ? g_nl++ : MAXL - 1];
    l->t[0] = 0; l->c = c; l->prio = prio; l->bar = 0;
    return l->t;
}

/* 1100..2300 Hz scale with the Robot 36 landmarks and the live tone */
static void tuning_bar(int x, int y, int w, int tone, int active)
{
    static const int marks[4] = { 1200, 1500, 1900, 2300 };
    int i, h = 12;
    lfill(x, y, w, h, 0xFF0C0E12u);
    lframe(x, y, w, h, C_DIM);
    for (i = 0; i < 4; i++) {
        int mx = x + 1 + (marks[i] - 1100) * (w - 2) / 1200;
        lfill(mx, y + 1, 1, h - 2, i == 0 ? 0xFF6080FFu : 0xFF505868u);
    }
    if (active && tone >= 1100 && tone <= 2300) {
        int tx = x + 1 + (tone - 1100) * (w - 2) / 1200;
        lfill(tx - 1, y - 2, 3, h + 4, C_WARN);
    }
}

/* copy picture rows whose contents changed (all rows when g_full) */
static void present_picture(void)
{
    int x, y, src = g_have_still ? 1 : 0;
    unsigned rev = g_view_rev;
    if (!g_full && src == g_cur->pic_src && rev == g_cur->pic_rev) return;
    g_cur->pic_src = src; g_cur->pic_rev = rev;
    for (y = 0; y < SSTV_H && y < g_lh; y++) {
        uint32_t h = 2166136261u, px;
        for (x = 0; x < SSTV_W; x++) h = (h ^ pic_px(y * SSTV_W + x)) * 16777619u;
        if (!g_full && h == g_cur->row[y]) continue;
        g_cur->row[y] = h;
        for (x = 0; x < SSTV_W && x < g_lw; x++) {
            px = pic_px(y * SSTV_W + x);
            *fb_at(x, y) = 0xFF000000u | px;
        }
    }
}
/* copy changed 16-px segments of the strip shadow */
static void present_strip(void)
{
    int y, sgi, x;
    for (y = 0; y < SSTV_H && y < g_lh; y++)
        for (sgi = 0; sgi < NSEG; sgi++) {
            const uint32_t *src = g_pb + y * STRIP_W + sgi * SEG_W;
            uint32_t h = 2166136261u;
            for (x = 0; x < SEG_W; x++) h = (h ^ src[x]) * 16777619u;
            if (!g_full && h == g_cur->seg[y][sgi]) continue;
            g_cur->seg[y][sgi] = h;
            for (x = 0; x < SEG_W; x++) *fb_at(STRIP_X + sgi * SEG_W + x, y) = src[x];
        }
}

/* Drain the capture and run the decoder. Called several times per frame
 * (start, after the picture, after the strip) so the gap between polls stays
 * under the 46 ms capture ring even when a frame is slow. */
static void audio_poll(void)
{
    if (g_listening && !g_rx) listen_stop();           /* decoder lost (rebuild OOM) */
    if (g_listening && g_rx) {
        uint32_t tn = now_us(), fr;
        ncap_poll(&g_cap, tn, on_audio, on_gap, 0);
        fr = g_cap.frames_out + g_cap.frames_lost;
        if (!g_rate_started) { if (fr) { g_rate_started = 1; g_rate_t0 = tn; g_rate_f0 = fr; } }
        else {
            uint32_t dus = tn - g_rate_t0;
            /* frames per second over >= 4 s, to the microsecond (DMA position
             * is exact at each poll, so this is good to ~10 ppm) */
            if (dus >= 4000000u) g_rate = (uint32_t)(((uint64_t)(fr - g_rate_f0) * 1000000u + dus / 2u) / dus);
            if (dus >= 20000000u) { g_rate_t0 = tn; g_rate_f0 = fr; }
        }
        if (g_rec && g_rec_n >= REC_FRAMES) rec_finish();
        /* first rate measurement: if the codec isn't at the rate the decoder
         * assumes (> 0.5 % off), rebuild the decoder for the measured rate.
         * (Tones and timings scale together with the rate: 8 % off moves the
         * 1900 Hz VIS leader out of the +-150 Hz window and stretches the
         * 150 ms line grid, so nothing would ever start.) */
        if (g_rate && !g_rate_checked && g_rx) {
            SstvStatus cs;
            sstv_status(g_rx, &cs);
            if (cs.state == SSTV_IDLE) {
                uint32_t d = g_rate > g_dec_rate ? g_rate - g_dec_rate : g_dec_rate - g_rate;
                g_rate_checked = 1;
                if (d * 200u > g_dec_rate) {
                    uint32_t want = g_rate;
                    sstv_free(g_rx);
                    g_rx = sstv_new((int)want);
                    if (g_rx) { g_dec_rate = want; g_images_seen = 0; say("Rate adjusted:", "decoder rebuilt", C_WARN); }
                    else { g_rx = sstv_new((int)g_dec_rate); say("Rate off, no memory", "", C_ERR); }
                    if (g_rx && g_ppm_known) sstv_set_ppm_prior(g_rx, g_ppm_prior);
                }
            }
        }
    }
}

/* ------------------------------------------------------------ frame */
void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    hb_trace_init();
    hb_trace_log("SSTVRX", 0, 0);
    prev_down = 0;
    g_last_st.last_vis = -1;                           /* no VIS heard yet */
    say("Tap Listen", "", C_DIM);
}

void hb_raw_frame(const hb_spoint_t *t)
{
    int down = TOUCH_DOWN(t), press = down && !prev_down, tx = -1, ty = -1, y;
    SstvStatus st;
    char *p;
    const int X = PANEL_X + 2, PW = 106;

    uint32_t t_in = now_us();
    if (g_fr_last) {
        uint32_t dt = t_in - g_fr_last;
        g_fr_avg = g_fr_avg ? g_fr_avg + (dt >> 3) - (g_fr_avg >> 3) : dt;
        if (dt > g_fr_max) g_fr_max = dt;
        if (dt > g_tick_fmax) g_tick_fmax = dt;
    }
    g_fr_last = t_in;

    g_fb = hb_raw_fb(); g_w = hb_raw_w(); g_h = hb_raw_h();
    g_lw = g_h; g_lh = g_w;
    if (!g_fonts_tried) fonts_load();
    /* which buffer is this, and does it still hold what we left in it? */
    g_cur = g_slot[0].fb == g_fb ? &g_slot[0] : g_slot[1].fb == g_fb ? &g_slot[1] : 0;
    if (!g_cur) {                                       /* new buffer: take a slot */
        g_cur = &g_slot[g_slot_next]; g_slot_next ^= 1;
        g_cur->fb = g_fb; g_cur->full = 1;
    }
    if (g_cur->rot != g_rot) g_cur->full = 1;
    if (!g_cur->full && *fb_at(STRIP_X + STRIP_W - 1, SSTV_H - 1) != C_SENT) {
        g_cur->full = 1;
        if (++g_sent_miss >= 3) g_persist = 0;         /* it doesn't: always redraw */
    } else if (!g_cur->full) g_sent_miss = 0;
    g_full = g_cur->full || !g_persist;
    g_cur->rot = g_rot;

    if (down || !g_touch_t) g_touch_t = t_in;
    audio_poll();
    if (!g_heap_t || t_in - g_heap_t >= HEAP_POLL_US) {
        g_heap_t = t_in;
        g_heap = hb_os_heap_free();
        if (g_heap < g_heap_min) g_heap_min = g_heap;
    }
    if (g_rx) {
        sstv_status(g_rx, &st);
        if (st.images != g_images_seen) {               /* an image just completed */
            g_images_seen = st.images;
            if (g_autosave) save_bmp();
        }
    } else st = g_last_st;
    vu_update(t_in);
    g_tick_frames++;
    if (g_listening && t_in - g_tick_t >= TICK_US) {           /* breadcrumbs (see top) */
        uint32_t idle = (t_in - g_touch_t) / 1000000u, fm = g_tick_fmax / 1000u, wm = g_tick_wmax / 1000u;
        uint32_t sy = st.sync_seen ? (uint32_t)(st.sync_good * 100 / st.sync_seen) : 0u;
        g_tick_t = t_in;
        hb_trace_log("TICK", (idle > 0xFFFFu ? 0xFFFFu : idle) << 16 | (fm > 0xFFFFu ? 0xFFFFu : fm),
                     (g_tick_frames > 0xFFFFu ? 0xFFFFu : g_tick_frames) << 16 | (wm > 0xFFFFu ? 0xFFFFu : wm));
        hb_trace_log("DEC", (uint32_t)st.state << 28 | (uint32_t)(st.last_vis >= 0 ? st.last_vis & 0xFF : 0xFF) << 16 |
                     ((uint32_t)st.line & 0xFFFFu), (uint32_t)(st.images & 0xFFFFu) << 16 | (uint32_t)(st.relocks & 0xFF) << 8 | sy);
        if (++g_tick_n % 3u == 0u) hb_trace_log("HEAP", g_heap, g_heap_min);
        g_tick_frames = 0; g_tick_fmax = 0; g_tick_wmax = 0;
    }

    /* ---- draw ---- */
    view_update();
    {   /* version splash while nothing has been received and we're not listening */
        int want = !g_listening && !g_have_still;
        if (want && !g_splash_on) { splash_draw(); g_splash_on = 1; g_view_rev++; }
        else if (!want && g_splash_on) {
            int i;
            g_splash_on = 0;
            if (!g_have_still) for (i = 0; i < SSTV_W * SSTV_H; i++) g_view[i] = 0;
            g_view_rev++;
        }
    }
    present_picture();
    audio_poll();
    {   int i; for (i = 0; i < SSTV_H * STRIP_W; i++) g_pb[i] = C_PANEL; }
    if (g_listening && st.state == SSTV_RX) {          /* progress tick at the line being drawn */
        int ly = st.height ? g_vy + st.line * g_vh / st.height : st.line;
        if (ly >= SSTV_H) ly = SSTV_H - 1;
        lfill(SSTV_W, ly, 2, 1, C_WARN);
    }

    {
        int lh = ui_line_h(FONT_UI), th = ui_line_h(FONT_TITLE);
        int sh = lh + 4 > 20 ? lh + 4 : 20, bh = th + 4 > 24 ? th + 4 : 24;   /* button heights */
        int r3 = SSTV_H - 2 - sh, r2 = r3 - 3 - sh, r1 = r2 - 4 - bh;        /* rows, bottom up */
        int i, total, top;

        ui_text(X, 2, FONT_TITLE, ui_text_w(FONT_TITLE, APP_NAME) <= PW ? APP_NAME : APP_SHORT, C_FG, PW);
        top = 2 + th + 2;

        g_nl = 0;
        p = ln_add(10, C_DIM);                                   /* state */
        if (!g_listening) put_s(p, "Off");
        else if (st.state == SSTV_RX) {
            g_ln[g_nl - 1].c = C_OK;
            p = put_s(p, "RX "); p = put_u(p, (uint32_t)st.line); p = put_s(p, "/"); put_u(p, (uint32_t)st.height);
        } else { g_ln[g_nl - 1].c = C_OK; put_s(p, "Listening"); }
        if (st.last_vis >= 0 && !st.last_vis_ok && st.state != SSTV_RX) {   /* mode */
            const char *nm = sstv_vis_name(st.last_vis);
            p = put_s(ln_add(9, C_WARN), "Skip ");
            if (nm) put_s(p, nm); else { p = put_s(p, "VIS "); put_u(p, (uint32_t)st.last_vis); }
        } else if (st.start_kind != SSTV_START_NONE)
        {
            p = put_s(ln_add(9, C_FG), st.mode ? st.mode : "?");
            put_s(p, st.start_kind == SSTV_START_VIS ? " VIS" : " sync");
        }
        else put_s(ln_add(9, C_DIM), "Waiting for VIS");
        p = ln_add(7, C_DIM);                                    /* sync + offset */
        p = put_s(p, "Sync ");
        p = put_u(p, st.sync_seen ? (uint32_t)(st.sync_good * 100 / st.sync_seen) : 0u);
        p = put_s(p, "% "); p = put_i(p, st.offset_hz); put_s(p, "Hz");
        p = ln_add(6, st.ppm_valid ? C_FG : C_DIM);              /* clock */
        p = put_s(p, "Clock "); p = put_i(p, st.ppm); put_s(p, st.ppm_valid ? "ppm" : "ppm?");
        ln_add(8, 0); g_ln[g_nl - 1].bar = 1;                    /* tuning bar */
        ln_add(8, 0); g_ln[g_nl - 1].bar = 2;                    /* level meter */
        p = ln_add(7, !g_listening ? C_DIM : g_vu_clip ? C_ERR : g_vu_pk_db10 > -30 ? C_FG : C_DIM);  /* level + tone */
        p = put_s(p, "Lvl ");
        if (g_listening && g_vu_clip) p = put_s(p, "CLIP");
        else if (g_listening && g_vu_pk_db10 > -600) {          /* peak, dBFS (0 dB = 32768) */
            int db = (g_vu_pk_db10 - 5) / 10;
            if (db < 0) p = put_i(p, db); else p = put_s(p, "0");
            p = put_s(p, "dB");
        } else p = put_s(p, "--");
        if (g_listening) { p = put_s(p, " "); p = put_u(p, (uint32_t)st.tone_hz); put_s(p, "Hz"); }
        if (g_listening) {
            /* capture health: drain mode, worst poll gap, audio lost */
            p = ln_add(5, g_cap.frames_lost ? C_WARN : g_cap.mode == NCAP_MODE_DMAPOS ? C_OK : C_WARN);
            if (g_cap.mode == NCAP_MODE_DMAPOS) { p = put_s(p, "DMA"); p = put_u(p, (uint32_t)g_cap.dma_ch); }
            else p = put_s(p, "Cursor");
            p = put_s(p, " "); p = put_u(p, g_cap.max_gap_us / 1000u);
            p = put_s(p, "ms lost "); put_u(p, g_cap.frames_lost / 44u);
            /* measured rate (* = decoder rebuilt for it); frame interval and
             * our work per frame avg/max ms; I/F = incremental/full redraw */
            p = ln_add(3, g_fr_max > 45000u ? C_WARN : C_DIM);
            if (g_rate) { p = put_u(p, g_rate); if (g_dec_rate != NCAP_RATE) p = put_s(p, "*"); }
            else p = put_s(p, "...");
            p = put_s(p, " F"); p = put_u(p, g_fr_avg / 1000u); p = put_s(p, "/"); p = put_u(p, g_fr_max / 1000u);
            p = put_s(p, " W"); p = put_u(p, g_wk_avg / 1000u); p = put_s(p, "/"); p = put_u(p, g_wk_max / 1000u);
            put_s(p, g_persist ? " I" : " F");
        }
        p = ln_add(6, g_heap < SAVE_FLOOR ? C_ERR : g_heap < REC_FLOOR ? C_WARN : C_DIM);   /* OS heap */
        p = put_s(p, "Heap "); p = put_u(p, (g_heap + 512u) / 1024u);       /* free now / lowest, KB */
        p = put_s(p, "/"); put_k(p, g_heap_min);
        p = ln_add(4, C_DIM);                                    /* images */
        p = put_s(p, "Img "); p = put_u(p, (uint32_t)(g_images_base + (g_rx ? st.images : 0u)));
        p = put_s(p, " saved "); put_u(p, (uint32_t)g_saved);
        if (g_msg[0][0]) put_s(ln_add(8, g_msg_col ? g_msg_col : C_DIM), g_msg[0]);
        if (g_msg[1][0]) put_s(ln_add(6, g_msg_col ? g_msg_col : C_DIM), g_msg[1]);

        /* drop lowest-priority lines until the rest fit above the buttons */
        for (;;) {
            int lo = -1;
            for (total = 0, i = 0; i < g_nl; i++)
                if (g_ln[i].prio >= 0) total += g_ln[i].bar ? 16 : lh;
            if (top + total <= r1 - 2) break;
            for (i = 0; i < g_nl; i++)
                if (g_ln[i].prio >= 0 && (lo < 0 || g_ln[i].prio < g_ln[lo].prio)) lo = i;
            if (lo < 0) break;
            g_ln[lo].prio = -1;
        }
        y = top;
        for (i = 0; i < g_nl; i++) {
            if (g_ln[i].prio < 0) continue;
            if (g_ln[i].bar == 1) { tuning_bar(X, y + 2, PW, st.tone_hz, g_listening); y += 16; }
            else if (g_ln[i].bar == 2) { vu_bar(X, y + 2, PW); y += 16; }
            else { ui_text(X, y, FONT_UI, g_ln[i].t, g_ln[i].c, PW); y += lh; }
        }

        /* ---- controls ---- */
        if (down) touch_l(t, &tx, &ty);
        if (button(down, press, tx, ty, X, r1, PW, bh, g_listening ? "Stop" : "Listen", FONT_TITLE,
                   g_listening ? C_BTN_ON : C_BTN)) {
            if (g_listening) listen_stop(); else listen_start();
        }
        if (button(down, press, tx, ty, X, r2, 34, sh, "Save|Sv", FONT_UI, C_BTN)) save_bmp();
        if (button(down, press, tx, ty, X + 36, r2, 34, sh, g_rec ? "..." : "Rec", FONT_UI, g_rec ? C_BTN_ON : C_BTN)) {
            if (g_rec) rec_finish(); else rec_start();
        }
        if (button(down, press, tx, ty, X + 72, r2, 34, sh, "Clear|Clr", FONT_UI, C_BTN)) {
            if (g_rx) sstv_clear(g_rx);
            { int i; for (i = 0; i < SSTV_W * SSTV_H; i++) g_view[i] = 0; g_view_rev++; }
            g_have_still = 0; g_src_w = g_src_h = 0;     /* (not listening: the splash comes back) */
            say("Cleared", "", C_DIM);
        }
        if (button(down, press, tx, ty, X, r3, 52, sh, g_autosave ? "Auto on|Auto" : "Auto off|Man", FONT_UI,
                   g_autosave ? C_BTN_ON : C_BTN)) g_autosave = !g_autosave;
        if (button(down, press, tx, ty, X + 54, r3, 52, sh, "Rotate|Rot", FONT_UI, C_BTN)) g_rot = !g_rot;
    }

    /* ---- out to the framebuffer: only what changed ---- */
    g_pb[(SSTV_H - 1) * STRIP_W + STRIP_W - 1] = C_SENT;
    present_strip();
    g_cur->full = 0;
    audio_poll();

    prev_down = down;
    {
        uint32_t wk = now_us() - t_in;
        g_wk_avg = g_wk_avg ? g_wk_avg + (wk >> 3) - (g_wk_avg >> 3) : wk;
        if (wk > g_wk_max) g_wk_max = wk;
        if (wk > g_tick_wmax) g_tick_wmax = wk;
    }
}
