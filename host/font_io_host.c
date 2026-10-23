/* SPDX-License-Identifier: GPL-3.0-only */
/* font_io_host.c -- font_file_load() for the host preview: FONT_DIR/x is read
 * from $SSTV_FONT_DIR/x (no fonts if the variable is unset). Same 64 KB limit
 * and static buffers as the iPod side. */
#include "font_io.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t font_mem[2][FONT_FILE_MAX];

const uint8_t *font_file_load(int slot, const char *file, uint32_t *size)
{
    const char *dir = getenv("SSTV_FONT_DIR");
    char p[1024];
    FILE *f;
    long n;
    *size = 0;
    if (!dir || slot < 0 || slot > 1) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, file);
    if (!(f = fopen(p, "rb"))) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)FONT_FILE_MAX || fread(font_mem[slot], 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    *size = (uint32_t)n;
    return font_mem[slot];
}
