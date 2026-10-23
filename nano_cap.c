/* SPDX-License-Identifier: GPL-3.0-only */
/* nano_cap.c -- see nano_cap.h. Integer only. */
#include "nano_cap.h"

#define HALF        NCAP_HALF_FRAMES
#define RING        NCAP_RING_FRAMES
#define RMASK       (RING - 1u)
#define HOLD        8u                  /* frames left behind the DMA pointer (DMAC FIFO / write buffer) */
#define KEEP        (RING - 64u)        /* readable span behind the pointer: 1.45 ms of slack vs. the
                                           ~0.2 ms the copy takes, so polls up to ~45 ms are gapless */
#define HALF_US     23220u              /* 1024 / 44100 s */
#define SCAN_EVERY  64u                 /* polls between channel re-scans when none is matched */
#define ADDR_MASK   0x0FFFFFFFu         /* compare cached/uncached/physical aliases of one buffer */

static uint32_t frames_in_us(uint32_t us)   /* us * 44100 / 1e6, no 64-bit, exact to 1 frame */
{
    return (us / 10000u) * 441u + ((us % 10000u) * 441u) / 10000u;
}

static int in_half(uint32_t addr, uint32_t base, uint32_t *off_bytes)
{
    uint32_t a = addr & ADDR_MASK, b = base & ADDR_MASK;
    if (b == 0 || a < b || a > b + HALF * 4u) return 0;
    *off_bytes = a - b;
    return 1;
}

/* PL080 C1: an enabled channel whose DstAddr lies inside one of our halves. */
static void scan_dma(NanoCap *c)
{
    int ch;
    uint32_t off;
    c->rescans++;
    c->dma_ch = -1;
    for (ch = 0; ch < 8; ch++) {
        uint32_t cfg = c->bus.rd32(c->bus.ctx, NCAP_PL080_CH(ch) + 0x10u);
        uint32_t dst;
        if (!(cfg & 1u)) continue;
        dst = c->bus.rd32(c->bus.ctx, NCAP_PL080_CH(ch) + 0x04u);
        if (in_half(dst, c->b[0], &off) || in_half(dst, c->b[1], &off)) {
            c->dma_ch = ch;
            c->dma_src = c->bus.rd32(c->bus.ctx, NCAP_PL080_CH(ch));
            return;
        }
    }
}

/* Live write position in ring frames, or -1 if the channel no longer points
 * into our halves. */
static int dma_wpos(NanoCap *c, uint32_t *wpos)
{
    uint32_t dst, off;
    if (c->dma_ch < 0) return -1;
    dst = c->bus.rd32(c->bus.ctx, NCAP_PL080_CH(c->dma_ch) + 0x04u);
    if (in_half(dst, c->b[0], &off)) { *wpos = (off / 4u) & RMASK; return 0; }
    if (in_half(dst, c->b[1], &off)) { *wpos = (HALF + off / 4u) & RMASK; return 0; }
    return -1;
}

static int cursor_half(NanoCap *c)
{
    uint32_t cur = c->bus.rd32(c->bus.ctx, NCAP_DEV_CUR) & ADDR_MASK;
    if (cur == (c->b[0] & ADDR_MASK)) return 0;
    if (cur == (c->b[1] & ADDR_MASK)) return 1;
    return -1;
}

void ncap_init(NanoCap *c, const NcapBus *bus)
{
    uint8_t *p = (uint8_t *)c;
    uint32_t i;
    for (i = 0; i < sizeof *c; i++) p[i] = 0;
    c->bus = *bus;
    c->dma_ch = -1;
    c->last_half = -1;
}

int ncap_arm(NanoCap *c, uint32_t now_us)
{
    if (!c->bus.dev_start(c->bus.ctx, 1)) return NCAP_E_NOTPRIMED;
    c->b[0] = c->bus.rd32(c->bus.ctx, NCAP_DEV_B0);
    c->b[1] = c->bus.rd32(c->bus.ctx, NCAP_DEV_B1);
    if (!c->b[0] || !c->b[1]) { c->bus.dev_start(c->bus.ctx, 0); return NCAP_E_NOTPRIMED; }
    c->armed = 1;
    c->started = 0;
    c->last_us = now_us;
    c->frames_out = c->frames_lost = c->overruns = c->polls = c->max_gap_us = c->rescans = 0;
    scan_dma(c);
    c->mode = c->dma_ch >= 0 ? NCAP_MODE_DMAPOS : NCAP_MODE_CURSOR;
    return NCAP_OK;
}

void ncap_disarm(NanoCap *c)
{
    if (!c->armed) return;
    c->bus.dev_start(c->bus.ctx, 0);
    c->armed = 0;
    c->mode = NCAP_MODE_NONE;
}

/* Fold `n` ring frames starting at rpos to mono into c->mono, advance rpos. */
static uint32_t gather(NanoCap *c, uint32_t n)
{
    uint32_t got = 0, pk = 0;
    while (got < n) {
        uint32_t h = c->rpos / HALF, off = c->rpos % HALF, cnt = HALF - off, i;
        const int16_t *src = c->bus.map(c->bus.ctx, c->b[h]);
        if (cnt > n - got) cnt = n - got;
        src += off * 2u;
        for (i = 0; i < cnt; i++) {
            int16_t v = src[2u * i];
            uint32_t a = (uint32_t)(v < 0 ? -(int32_t)v : v);
            if (a > pk) pk = a;
            c->mono[got + i] = v;
        }
        got += cnt;
        c->rpos = (c->rpos + cnt) & RMASK;
    }
    c->peak = pk;
    return got;
}

static int poll_dma(NanoCap *c, uint32_t dt, ncap_sink_fn sink, ncap_gap_fn gap, void *user)
{
    uint32_t w, adv, expect, held, pending, n;
    if (dma_wpos(c, &w) < 0) return -1;
    if (!c->started) {                          /* first look: start from here */
        c->started = 1;
        c->wpos = w;
        c->rpos = (w - HOLD) & RMASK;
        return 0;
    }
    held = (c->wpos - c->rpos) & RMASK;         /* written but not taken last time (<= KEEP) */
    /* how far the DMA went since the last poll: the ring tells us mod 2048,
     * the clock tells us how many whole laps to add */
    adv = (w - c->wpos) & RMASK;
    expect = frames_in_us(dt);
    if (expect > adv + RING / 2u) adv += ((expect - adv + RING / 2u) / RING) * RING;
    c->wpos = w;
    pending = held + adv;                       /* true amount written and unread */
    if (pending > KEEP) {                       /* overrun: the oldest part was overwritten */
        uint32_t lost = pending - KEEP;
        c->frames_lost += lost;
        c->overruns++;
        c->rpos = (c->wpos - KEEP) & RMASK;
        pending = KEEP;
        if (gap) gap(user, (int)lost);
    }
    if (pending <= HOLD) return 0;
    n = gather(c, pending - HOLD);
    c->frames_out += n;
    if (sink) sink(user, c->mono, (int)n);
    return (int)n;
}

/* ---- cursor fallback: track the phase of the 23.22 ms flip grid ----
 * A flip seen between two polls only says it happened somewhere in
 * (t_prev, t_now]. Intersecting those windows over many polls pins the grid
 * down to the poll jitter, and then the number of flips between any two polls
 * -- including the ones we slept through -- follows from the clock, with the
 * cursor's parity (changed = odd) as the check. */
static int32_t fdiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int32_t fmodp(int32_t a, int32_t b) { int32_t r = a % b; return r < 0 ? r + b : r; }
static int flips_for(uint32_t x, uint32_t tp, uint32_t tn)
{
    return (int)(fdiv((int32_t)(tn - x), (int32_t)HALF_US) - fdiv((int32_t)(tp - x), (int32_t)HALF_US));
}

static int poll_cursor(NanoCap *c, uint32_t now_us, uint32_t dt, ncap_sink_fn sink, ncap_gap_fn gap, void *user)
{
    int h = cursor_half(c), changed, n = -1;
    uint32_t tp = now_us - dt, got;
    if (h < 0) return 0;
    if (!c->started || c->last_half < 0) {
        c->started = 1; c->last_half = h; c->ph_valid = 0;
        return 0;
    }
    changed = (h != c->last_half);
    if (c->ph_valid) {
        /* pieces of [a, b] split where the count of grid points in (tp, tn] changes */
        uint32_t a = c->ph_a, b = c->ph_b, cut[4], lo = 0, hi = 0;
        int nc = 0, i, k, have = 0, want = changed ? 1 : 0;
        uint32_t ep = a + (uint32_t)fmodp((int32_t)(tp - a), (int32_t)HALF_US);
        uint32_t en = a + (uint32_t)fmodp((int32_t)(now_us - a), (int32_t)HALF_US);
        cut[nc++] = a;
        if ((int32_t)(ep - a) > 0 && (int32_t)(ep - b) <= 0) cut[nc++] = ep;
        if ((int32_t)(en - a) > 0 && (int32_t)(en - b) <= 0) cut[nc++] = en;
        if (nc == 3 && (int32_t)(cut[2] - cut[1]) < 0) { uint32_t t = cut[1]; cut[1] = cut[2]; cut[2] = t; }
        cut[nc] = b + 1u;
        {
            int ok[3] = { 0, 0, 0 }, first = -1, last = -1, gapmid = 0;
            for (i = 0; i < nc; i++) {
                uint32_t p0 = cut[i], p1 = cut[i + 1] - 1u;
                if ((int32_t)(p1 - p0) < 0) continue;
                k = flips_for(p0 + (p1 - p0) / 2u, tp, now_us);
                if ((k & 1) != want) continue;
                if (n >= 0 && k != n) continue;       /* (pieces differ by at most 1) */
                n = k; ok[i] = 1;
                if (first < 0) first = i;
                last = i;
            }
            for (i = first + 1; first >= 0 && i < last; i++) if (!ok[i]) gapmid = 1;
            if (first >= 0) {
                have = 1;
                if (gapmid && (int32_t)(b - a) >= (int32_t)HALF_US - 8) {
                    /* [a, b] is the whole circle: the two end pieces are one arc */
                    lo = cut[last]; hi = cut[first + 1] - 1u + HALF_US;
                } else {
                    lo = cut[first]; hi = cut[last + 1] - 1u;
                }
            }
        }
        if (have) { c->ph_a = lo; c->ph_b = hi; }
        else c->ph_valid = 0;                         /* inconsistent: re-learn the phase */
    }
    if (!c->ph_valid) {
        if (!changed) {
            /* no phase yet, no change: only a long sleep could hide flips */
            if (dt < HALF_US + HALF_US / 2u) { return 0; }
            n = (int)((dt + HALF_US / 2u) / HALF_US) & ~1;
        } else {
            n = (int)((dt + HALF_US / 2u) / HALF_US) | 1;
            /* the most recent flip is in (tp, now]; keep it if that's < one period */
            c->ph_b = now_us;
            c->ph_a = dt < HALF_US ? tp + 1u : now_us - HALF_US + 1u;
            c->ph_valid = 1;
        }
    }
    /* keep [a, b] near now (so differences stay small) */
    if (c->ph_valid) {
        int32_t kk = fdiv((int32_t)(now_us - c->ph_b), (int32_t)HALF_US);
        c->ph_a += (uint32_t)kk * HALF_US; c->ph_b += (uint32_t)kk * HALF_US;
        /* codec clock vs CPU clock: allow 1000 ppm of drift per poll interval */
        c->ph_a -= 2u + dt / 1000u; c->ph_b += 2u + dt / 1000u;
        if ((int32_t)(c->ph_b - c->ph_a) >= (int32_t)HALF_US) c->ph_a = c->ph_b - HALF_US + 1u;
    }
    c->last_half = h;
    if (n <= 0) return 0;
    if (n > 1) {
        uint32_t lost = (uint32_t)(n - 1) * HALF;
        c->frames_lost += lost; c->overruns++;
        if (gap) gap(user, (int)lost);
    }
    c->rpos = (uint32_t)(h ^ 1) * HALF;         /* the half that just finished */
    got = gather(c, HALF);
    c->frames_out += got;
    if (sink) sink(user, c->mono, (int)got);
    return (int)got;
}

int ncap_poll(NanoCap *c, uint32_t now_us, ncap_sink_fn sink, ncap_gap_fn gap, void *user)
{
    uint32_t dt;
    int r;
    if (!c->armed) return NCAP_E_STATE;
    dt = now_us - c->last_us;
    c->last_us = now_us;
    c->polls++;
    if (c->polls > 2u && dt > c->max_gap_us) c->max_gap_us = dt;

    if (c->mode == NCAP_MODE_DMAPOS) {
        r = poll_dma(c, dt, sink, gap, user);
        if (r >= 0) return r;
        /* channel moved or was reassigned: fall back to the cursor, rescan later.
         * Resume the cursor stream at the half the DMA was last in. */
        c->mode = NCAP_MODE_CURSOR; c->dma_ch = -1;
        c->last_half = (int)(c->wpos / HALF);
        c->ph_valid = 0;
    }
    /* cursor mode; keep looking for the channel (every poll at first) */
    if (c->polls < 16u || (c->polls % SCAN_EVERY) == 0u) {
        scan_dma(c);
        if (c->dma_ch >= 0) {
            c->mode = NCAP_MODE_DMAPOS;
            if (c->started && c->last_half >= 0) {
                /* mid-stream: everything from the start of the half that was
                 * filling at the last delivery is still unread */
                c->rpos = c->wpos = (uint32_t)c->last_half * HALF;
                r = poll_dma(c, dt, sink, gap, user);
                return r < 0 ? 0 : r;
            }
            c->started = 0;
            r = poll_dma(c, dt, sink, gap, user);
            return r < 0 ? 0 : r;
        }
    }
    return poll_cursor(c, now_us, dt, sink, gap, user);
}

/* ------------------------------------------------------------- the nano */
#ifdef APRICOT_NANO
static uint32_t nano_rd32(void *ctx, uint32_t a) { (void)ctx; return *(volatile uint32_t *)(uintptr_t)a; }
static const int16_t *nano_map(void *ctx, uint32_t a) { (void)ctx; return (const int16_t *)(uintptr_t)a; }
/* device-start: dev = 0x08971F0C, vt = *dev, fn = vt[0x3C/4], fn(dev, enable, 0).
 * Called only when the device is set up (its vtable pointer is non-zero). */
static int nano_dev_start(void *ctx, int enable)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)NCAP_DEV;
    uint32_t vt = d[0], fn;
    (void)ctx;
    if (vt == 0u) return 0;                          /* not primed this boot */
    fn = ((volatile uint32_t *)(uintptr_t)vt)[0x3Cu / 4u];
    ((int (*)(void *, uint32_t, uint32_t))(uintptr_t)(fn | 1u))((void *)d, (uint32_t)enable, 0u);
    return 1;
}
static const NcapBus g_nano_bus = { nano_rd32, nano_map, nano_dev_start, 0 };
const NcapBus *ncap_nano_bus(void) { return &g_nano_bus; }
#endif
