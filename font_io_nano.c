/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * font_io_nano.c -- font_file_load() on the iPod: hb_fs_size + hb_fs_read
 * into a static buffer.
 * The filesystem calls work only while the iPod's disk is mounted (home
 * screen, not Connected mode); otherwise they fail and the app keeps the
 * built-in font.
 */
#include "hb_sdk.h"
#include "font_io.h"

static uint8_t font_mem[2][FONT_FILE_MAX] __attribute__((aligned(8)));

const uint8_t *font_file_load(int slot, const char *file, uint32_t *size)
{
    char path[96];
    const char *d = FONT_DIR;
    uint32_t n, i = 0, j = 0;
    *size = 0;
    if (slot < 0 || slot > 1 || !file) return 0;
    while (d[j] && i < sizeof path - 1) path[i++] = d[j++];
    if (i < sizeof path - 1) path[i++] = '/';
    for (j = 0; file[j] && i < sizeof path - 1; j++) path[i++] = file[j];
    path[i] = 0;
    n = hb_fs_size(path);                       /* 0 = missing */
    if (n == 0 || n > FONT_FILE_MAX) return 0;
    if (hb_fs_read(path, font_mem[slot], n) != n) return 0;
    *size = n;
    return font_mem[slot];
}
