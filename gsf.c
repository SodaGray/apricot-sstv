/* SPDX-License-Identifier: GPL-3.0-only */
/* gsf.c -- see gsf.h */
#include "gsf.h"

#define GSF_GLYPH_SIZE 16u
#define GSF_REQUIRED   7u               /* MSB first | tight bits | sorted codepoints */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static void fixed_text(char *dst, uint32_t cap, const uint8_t *src, uint32_t len)
{
    uint32_t n = 0;
    while (n < len && n + 1 < cap && src[n]) { dst[n] = (char)src[n]; n++; }
    dst[n] = 0;
}

/* CRC-32 (IEEE), four bits per step: 16-entry table, 64 bytes */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu, 0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu };
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15u];
        c = (c >> 4) ^ t[c & 15u];
    }
    return ~c;
}

int gsf_header(const uint8_t *h, uint32_t len, GsfInfo *o)
{
    if (len < GSF_HEADER_SIZE) return -1;
    if (h[0] != 'G' || h[1] != 'S' || h[2] != 'F' || h[3] != '1') return -1;
    if (rd16(h + 4) != 1u || rd16(h + 6) != GSF_HEADER_SIZE) return -1;
    if ((rd32(h + 8) & GSF_REQUIRED) != GSF_REQUIRED) return -1;
    if (h[18] != 1u) return -1;                         /* 1 bpp only */
    if (rd32(h + 28) != GSF_GLYPH_SIZE) return -1;
    o->point_size  = (uint16_t)rd16(h + 12);
    o->line_height = (uint16_t)rd16(h + 14);
    o->base_line   = (int16_t)rd16(h + 16);
    o->glyph_count = rd32(h + 20);
    o->first_cp    = rd32(h + 40);
    o->last_cp     = rd32(h + 44);
    fixed_text(o->family, sizeof o->family, h + 52, 32);
    fixed_text(o->style, sizeof o->style, h + 84, 16);
    if (!o->glyph_count || o->line_height == 0 || o->line_height > 255u) return -1;
    return 0;
}

int gsf_open(Gsf *g, const uint8_t *d, uint32_t size)
{
    uint32_t i, prev = 0, bsize, table_end;
    if (gsf_header(d, size, &g->info)) return -1;
    g->data = d; g->size = size;
    g->table = rd32(d + 24);
    g->bitmap = rd32(d + 32);
    bsize = rd32(d + 36);
    if (g->table < GSF_HEADER_SIZE || g->table > size || g->info.glyph_count > (size - g->table) / GSF_GLYPH_SIZE) return -1;
    table_end = g->table + g->info.glyph_count * GSF_GLYPH_SIZE;
    if (table_end > g->bitmap || g->bitmap > size || bsize > size - g->bitmap) return -1;
    if (crc32(d + g->table, g->bitmap + bsize - g->table) != rd32(d + 48)) return -1;

    for (i = 0; i < 95; i++) g->ascii[i] = 0xffffu;
    for (i = 0; i < g->info.glyph_count; i++) {
        const uint8_t *r = d + g->table + i * GSF_GLYPH_SIZE;
        uint32_t cp = rd32(r), rel = rd32(r + 4), bs = rd16(r + 14);
        if (i && cp <= prev) return -1;
        if (bs != ((uint32_t)r[10] * r[11] + 7u) / 8u || rel > bsize || bs > bsize - rel) return -1;
        if (cp >= 0x20u && cp <= 0x7eu && i < 0xffffu) g->ascii[cp - 0x20u] = (uint16_t)i;
        prev = cp;
    }
    return 0;
}

static void fill_glyph(const Gsf *g, uint32_t i, GsfGlyph *o)
{
    const uint8_t *r = g->data + g->table + i * GSF_GLYPH_SIZE;
    o->bits    = g->data + g->bitmap + rd32(r + 4);
    o->advance = (uint16_t)rd16(r + 8);
    o->box_w   = r[10];
    o->box_h   = r[11];
    o->ofs_x   = (int8_t)r[12];
    o->ofs_y   = (int8_t)r[13];
}

int gsf_glyph(const Gsf *g, uint32_t cp, GsfGlyph *o)
{
    uint32_t lo = 0, hi = g->info.glyph_count;
    if (cp >= 0x20u && cp <= 0x7eu) {
        uint32_t i = g->ascii[cp - 0x20u];
        if (i == 0xffffu) return 0;
        fill_glyph(g, i, o);
        return 1;
    }
    while (lo < hi) {                               /* sorted: binary search */
        uint32_t mid = (lo + hi) / 2u, c = rd32(g->data + g->table + mid * GSF_GLYPH_SIZE);
        if (c == cp) { fill_glyph(g, mid, o); return 1; }
        if (c < cp) lo = mid + 1u; else hi = mid;
    }
    return 0;
}
