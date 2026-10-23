/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * font_io.h -- the app's GSF1 fonts: which files, and where their bytes live.
 *
 * Fonts are GSF1 files in /Apps/Data/Fonts. Each role tries its candidates
 * in order and takes the first that loads and validates (gsf.c); if none
 * does, that role is drawn in the built-in 5x7. A file is read whole into a
 * 64 KB static buffer of its own, in the app's .bss -- not the OS heap, which
 * the app can't free when it's quit -- so it stays valid for the whole run.
 */
#ifndef SSTV_FONT_IO_H
#define SSTV_FONT_IO_H
#include <stdint.h>

#define FONT_DIR        "/Apps/Data/Fonts"
#define FONT_FILE_MAX   (64u * 1024u)

/* candidates per role, first that loads wins (comma-separated in the source) */
#define SSTV_FONTS_UI    { "Geneva-9.gsf", "Monaco-9.gsf", "Geneva-12.gsf", 0 }    /* status lines */
#define SSTV_FONTS_TITLE { "Chicago-12.gsf", "Geneva-12.gsf", 0 }                   /* title, Listen/Stop */

/* Read FONT_DIR/file into slot's static buffer. Returns the bytes and sets
 * *size, or 0 if the file is missing, empty, or larger than FONT_FILE_MAX. */
const uint8_t *font_file_load(int slot, const char *file, uint32_t *size);

#endif
