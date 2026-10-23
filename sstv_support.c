/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * sstv_support.c -- what the decoder needs on NanoApps (-nostdlib):
 *
 * - The decoder's memory: ONE block of SSTV_MEM_BYTES from the OS heap
 *   (sstv_pool_open at LISTEN, sstv_pool_close at STOP), handed out by
 *   sstv_pool_alloc/free as a stack. sstv_new takes all its blocks at once
 *   and sstv_free gives them back in reverse, so the pool is reused exactly
 *   for each new decoder (rate rebuild included).
 *   Why not .bss: with the pool there, code + .bss came to ~1.0 MB and the
 *   system crashed at a fixed point part-way through long (PD240) pictures,
 *   consistent with the end of .bss lying past the memory the system gives
 *   an app. Keep code + .bss well under ~800 KB.
 * - word-wise memset/memcpy (gcc emits calls to these for struct copies and
 *   clears), WEAK so an SDK or libgcc definition wins if one exists.
 *
 * Soft-float, integer only. sstv_dsp.c is built with -DSSTV_POOL to use it.
 */
#include <stddef.h>
#include <stdint.h>
#include "sstv_dsp.h"

#define WEAK  __attribute__((weak))
#define NOLDP __attribute__((optimize("no-tree-loop-distribute-patterns")))

/* each block: 8-byte header {offset of the block below, freed flag}, then data */
typedef struct { uint32_t below; uint32_t freed; } PoolHdr;
void *hb_os_alloc(uint32_t size);
void  hb_os_free(void *p);
static uint8_t *g_pool;           /* 8-aligned, inside g_raw */
static void    *g_raw;
static uint32_t g_top;            /* first free byte */
static uint32_t g_last;           /* offset of the topmost block's header, if g_top */
static uint32_t g_peak;

void *sstv_pool_alloc(size_t n)
{
    uint32_t need;
    PoolHdr *h;
    if (!g_pool || n > SSTV_MEM_BYTES) return NULL;
    need = (uint32_t)(sizeof(PoolHdr) + ((n + 7u) & ~(size_t)7u));
    if (need > SSTV_MEM_BYTES - g_top) return NULL;
    h = (PoolHdr *)(g_pool + g_top);
    h->below = g_top ? g_last : 0xFFFFFFFFu;
    h->freed = 0;
    g_last = g_top;
    g_top += need;
    if (g_top > g_peak) g_peak = g_top;
    return h + 1;
}

void sstv_pool_free(void *p)
{
    if (!p) return;
    ((PoolHdr *)p - 1)->freed = 1;
    while (g_top) {                                  /* pop freed blocks off the top */
        PoolHdr *h = (PoolHdr *)(g_pool + g_last);
        if (!h->freed) break;
        g_top = g_last;
        if (h->below == 0xFFFFFFFFu) { g_top = 0; break; }
        g_last = h->below;
    }
}

uint32_t sstv_pool_peak(void) { return g_peak; }
void    *sstv_pool_base(void) { return g_pool; }

/* 1 if the pool is there (taken now or already), 0 if the OS heap refused */
int sstv_pool_open(void)
{
    if (g_pool) return 1;
    g_raw = hb_os_alloc(SSTV_MEM_BYTES + 8u);
    if (!g_raw) return 0;
    g_pool = (uint8_t *)(((uintptr_t)g_raw + 7u) & ~(uintptr_t)7u);
    g_top = 0; g_last = 0;
    return 1;
}
/* give the block back to the OS; only once every decoder block is freed */
void sstv_pool_close(void)
{
    if (!g_pool || g_top) return;
    hb_os_free(g_raw);
    g_raw = 0; g_pool = 0;
}

WEAK NOLDP void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d, v = (unsigned char)c;
    if (n >= 16) {
        uint32_t w = v * 0x01010101u, *q;
        while ((uintptr_t)p & 3u) { *p++ = v; n--; }
        q = (uint32_t *)p;
        while (n >= 16) { q[0] = w; q[1] = w; q[2] = w; q[3] = w; q += 4; n -= 16; }
        while (n >= 4) { *q++ = w; n -= 4; }
        p = (unsigned char *)q;
    }
    while (n--) *p++ = v;
    return d;
}

WEAK NOLDP void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (n >= 16 && !(((uintptr_t)a ^ (uintptr_t)b) & 3u)) {
        uint32_t *aw;
        const uint32_t *bw;
        while ((uintptr_t)a & 3u) { *a++ = *b++; n--; }
        aw = (uint32_t *)a; bw = (const uint32_t *)b;
        while (n >= 16) { uint32_t t0 = bw[0], t1 = bw[1], t2 = bw[2], t3 = bw[3];
                          aw[0] = t0; aw[1] = t1; aw[2] = t2; aw[3] = t3; aw += 4; bw += 4; n -= 16; }
        while (n >= 4) { *aw++ = *bw++; n -= 4; }
        a = (unsigned char *)aw; b = (const unsigned char *)bw;
    }
    while (n--) *a++ = *b++;
    return d;
}
