/* SPDX-License-Identifier: GPL-3.0-only */
/* host stub of the NanoApps SDK surface sstv_app.c uses -- for compile checks
 * and the UI preview only. Signatures as the app calls them. */
#ifndef HB_SDK_STUB_H
#define HB_SDK_STUB_H
#include <stdint.h>
typedef struct { int16_t x, y; int down; } hb_spoint_t;
uint64_t hb_time_uptime_us(void);
uint32_t hb_time_uptime_ms(void);
void hb_trace_init(void);
void hb_trace_log(const char *tag, uint32_t a, uint32_t b);
int  hb_fs_stream_open(const char *path);
int  hb_fs_stream_write(const void *buf, uint32_t len);
int  hb_fs_stream_close(void);
uint32_t hb_fs_size(const char *path);                          /* 0 = missing */
uint32_t hb_fs_read(const char *path, void *buf, uint32_t len);  /* bytes read, 0 = error */
void *hb_os_alloc(uint32_t size);
void  hb_os_free(void *p);
#define HB_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#endif
