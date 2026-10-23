/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nano_cap.h -- gapless drain of the iPod nano 7 (RetailOS 1.1.2) capture DMA,
 * for RX-only audio apps. INTEGER ONLY: lives in the soft-float app objects.
 *
 * The device (RetailOS 1.1.2): wired capture device 0x08971F0C, the one
 * Voice Memos uses, normally set up at boot. Its start method (vtable +0x3C,
 * fn(dev, 1, 0)) re-arms codec -> I2S0 RX -> PL080 -> two DMA halves at
 * device+0x90 / +0x94 (1024 stereo frames = 23.2 ms each, 44100 Hz, L == R);
 * device+0x98 holds the address of the half being filled.
 *
 * The problem this solves: the UI frame interval runs up to ~31 ms, longer
 * than a 23.2 ms half, so "drain the finished half when the cursor flips"
 * drops halves. Here the two halves are treated as ONE 2048-frame (46.4 ms)
 * ring, and the capture channel's live PL080 destination register (found by
 * scanning controller C1's channels for a DstAddr inside one of the halves;
 * read-only MMIO) gives the exact write position. Every poll takes everything
 * written since the last one, so any poll interval under ~40 ms is gapless.
 * Longer stalls are detected from the clock and reported as an exact count of
 * lost frames (feed them to sstv_gap so the picture's timing stays right).
 *
 * If no channel matches, it falls back to cursor-flip draining (one finished
 * half per flip, with flips inferred from the clock when polls are slow),
 * with the same loss accounting.
 *
 * Bus access goes through NcapBus, so the identical logic runs against a
 * simulated DMA on the host (host/test_cap.c).
 */
#ifndef NANO_CAP_H
#define NANO_CAP_H
#include <stdint.h>

#define NCAP_HALF_FRAMES 1024u
#define NCAP_RING_FRAMES (2u * NCAP_HALF_FRAMES)
#define NCAP_RATE        44100u

enum { NCAP_OK = 0, NCAP_E_NOTPRIMED = -1, NCAP_E_STATE = -2 };
enum { NCAP_MODE_NONE = 0, NCAP_MODE_DMAPOS = 1, NCAP_MODE_CURSOR = 2 };

typedef struct {
    uint32_t (*rd32)(void *ctx, uint32_t addr);              /* MMIO / DRAM word read */
    const int16_t *(*map)(void *ctx, uint32_t bus_addr);     /* capture half -> readable pointer */
    int (*dev_start)(void *ctx, int enable);                 /* device-start; 0 = not primed */
    void *ctx;
} NcapBus;

/* Delivers mono s16 (left channel) at 44100. */
typedef void (*ncap_sink_fn)(void *user, const int16_t *mono, int frames);
typedef void (*ncap_gap_fn)(void *user, int frames);

typedef struct {
    NcapBus  bus;
    int      armed, mode, dma_ch;       /* dma_ch: PL080 C1 channel, -1 = none */
    uint32_t b[2];                      /* half bus addresses (as the device holds them) */
    uint32_t last_us;                   /* time of the previous poll */
    uint32_t wpos;                      /* last DMA write position, ring frames [0, 2048) */
    uint32_t rpos;                      /* our read position, ring frames */
    int      last_half;                 /* cursor mode: last active half seen */
    int      ph_valid;                  /* cursor mode: flip-grid phase known ... */
    uint32_t ph_a, ph_b;                /* ... a flip happened at some time in [a, b] (us) */
    int      started;
    /* stats */
    uint32_t frames_out, frames_lost, overruns, polls, max_gap_us, rescans;
    uint32_t peak;                      /* |s16| peak of the last delivery */
    uint32_t dma_src;                   /* SrcAddr of the matched channel (diagnostic) */
    int16_t  mono[NCAP_RING_FRAMES];
} NanoCap;

void ncap_init(NanoCap *c, const NcapBus *bus);
/* Arm capture (device-start enable=1) and find the DMA channel. */
int  ncap_arm(NanoCap *c, uint32_t now_us);
void ncap_disarm(NanoCap *c);
/* Drain everything new. Call often (every frame). Returns frames delivered. */
int  ncap_poll(NanoCap *c, uint32_t now_us, ncap_sink_fn sink, ncap_gap_fn gap, void *user);

#ifdef APRICOT_NANO
/* The real bus: direct MMIO, firmware 1.1.2 device-start. */
const NcapBus *ncap_nano_bus(void);
#endif

/* addresses (RetailOS 1.1.2), exported for the host simulation */
#define NCAP_DEV          0x08971F0Cu
#define NCAP_DEV_B0       (NCAP_DEV + 0x90u)
#define NCAP_DEV_B1       (NCAP_DEV + 0x94u)
#define NCAP_DEV_CUR      (NCAP_DEV + 0x98u)
#define NCAP_PL080_C1     0x38200000u
#define NCAP_PL080_CH(n)  (NCAP_PL080_C1 + 0x100u + (uint32_t)(n) * 0x20u)   /* +0 Src +4 Dst +0x10 Cfg */

#endif /* NANO_CAP_H */
