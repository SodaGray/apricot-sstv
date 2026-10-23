/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HB_RAW_SURFACE_STUB_H
#define HB_RAW_SURFACE_STUB_H
#include <stdint.h>
uint32_t *hb_raw_fb(void);
int  hb_raw_w(void);
int  hb_raw_h(void);
void hb_raw_fill(uint32_t argb);
void hb_raw_fill_rect(int x, int y, int w, int h, uint32_t argb);
void hb_raw_rect_outline(int x, int y, int w, int h, int t, uint32_t argb);
void hb_raw_init(int w, int h);
void hb_raw_frame(const hb_spoint_t *t);
#endif
