/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * gsf.h -- freestanding reader for GSF1 bitmap fonts (1 bpp, tight bits,
 * MSB first, sorted codepoints), working on a font file loaded in memory.
 * Checks the header, record table and CRC32; no libc.
 */
#ifndef GSF_H
#define GSF_H
#include <stdint.h>

#define GSF_HEADER_SIZE 128u

/* what the 128-byte header says (enough for a font list) */
typedef struct {
    uint16_t point_size, line_height;
    int16_t  base_line;                 /* from the bottom of the line up to the baseline */
    uint32_t glyph_count, first_cp, last_cp;
    char family[33], style[17];
} GsfInfo;

typedef struct {
    const uint8_t *data;                /* whole file; owned by the caller */
    uint32_t size, table, bitmap;
    GsfInfo  info;
    uint16_t ascii[95];                 /* glyph index of 0x20..0x7E, 0xFFFF if missing */
} Gsf;

typedef struct {
    const uint8_t *bits;                /* box_w * box_h bits, row by row, MSB first */
    uint16_t advance;
    uint8_t  box_w, box_h;
    int8_t   ofs_x, ofs_y;              /* LVGL meaning: ofs_y = baseline to box bottom */
} GsfGlyph;

/* header only: 0 if it is a GSF1 file this reader can draw */
int gsf_header(const uint8_t *h, uint32_t len, GsfInfo *out);
/* full check of a loaded file (bounds, order, bitmap sizes, CRC): 0 if usable */
int gsf_open(Gsf *g, const uint8_t *data, uint32_t size);
/* 1 and *out filled if the font has codepoint cp */
int gsf_glyph(const Gsf *g, uint32_t cp, GsfGlyph *out);

#endif
