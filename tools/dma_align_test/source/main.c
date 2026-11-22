// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// dma_align_test: GSP DMAs (GX_RequestDma) at byte offsets, between linear memory (FCRAM) and
// VRAM. Each case fills its source with a pattern and its destination's span, with 8
// guard bytes each side, with 0xEE, DMAs, and compares: the span must hold the source's bytes and
// the guards 0xEE. Chained cases queue two DMAs back to back, the second reading what the first
// wrote. The bottom screen prints each case and a last line, ALL OK or the failed count; it runs
// the cases again every 2 s. Start quits.
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define GUARD 8
#define MAXSIZE 9000

typedef struct {
	const char* name;
	int src_vram, src_off, dst_vram, dst_off, size;
	// A second DMA, queued right after the first: from the first's destination (plus its offset)
	// into a third buffer (linear) at its offset, its size.
	int chain, chain_src_off, chain_dst_off, chain_size;
} Case;

static const Case cases[] = {
	{"L+2 > V+0 48 (PXZ2)", 0, 2, 1, 0, 48, 0, 0, 0, 0},
	{"L+1 > L+3 47", 0, 1, 0, 3, 47, 0, 0, 0, 0},
	{"L+3 > L+3 4099", 0, 3, 0, 3, 4099, 0, 0, 0, 0},
	{"V+2 > L+1 1000", 1, 2, 0, 1, 1000, 0, 0, 0, 0},
	{"L+1 > V+2 9000", 0, 1, 1, 2, 9000, 0, 0, 0, 0},
	{"L+0 > V+0 64 (aligned)", 0, 0, 1, 0, 64, 0, 0, 0, 0},
	{"L+0 > L+0 4103 (tail)", 0, 0, 0, 0, 4103, 0, 0, 0, 0},
	{"L+0>V+0 256, V+1>L+2 255", 0, 0, 1, 0, 256, 1, 1, 2, 255},
	{"L+2>V+0 48, V+0>L+0 48", 0, 2, 1, 0, 48, 1, 0, 0, 48},
};
#define CASES (sizeof(cases) / sizeof(cases[0]))

static u8* lin_src;
static u8* lin_dst;
static u8* lin_out;
static u8* vram_a;
static u8* vram_b;

// The span's bytes against want, and the guards; returns the first bad offset from the span's
// start (-GUARD..size+GUARD) or a value past it when every byte matches.
static int check(const u8* span, const u8* want, int size, int* got, int* expected) {
	for (int i = -GUARD; i < size + GUARD; i++) {
		const int e = (i < 0 || i >= size) ? 0xEE : want[i];
		if (span[i] != e) {
			*got = span[i];
			*expected = e;
			return i;
		}
	}
	return size + GUARD;
}

static void fill(u8* p, int n, u8 seed) {
	for (int i = 0; i < n; i++) {
		p[i] = (u8)(seed + i * 7 + (i >> 8));
	}
}

static int run_case(const Case* c, int k, u8 seed) {
	u8* src_buf = c->src_vram ? vram_a : lin_src;
	u8* dst_buf = c->dst_vram ? vram_b : lin_dst;
	u8* src = src_buf + 16 + c->src_off;
	u8* dst = dst_buf + 16 + c->dst_off;
	static u8 want[MAXSIZE + 2 * GUARD];
	fill(src, c->size, seed);
	memcpy(want, src, c->size);
	memset(dst - GUARD, 0xEE, c->size + 2 * GUARD);
	u8* out = lin_out + 16 + c->chain_dst_off;
	if (c->chain) {
		memset(out - GUARD, 0xEE, c->chain_size + 2 * GUARD);
		GSPGPU_FlushDataCache(out - GUARD, c->chain_size + 2 * GUARD);
	}
	GSPGPU_FlushDataCache(src_buf, MAXSIZE + 64);
	GSPGPU_FlushDataCache(dst - GUARD, c->size + 2 * GUARD);
	GX_RequestDma((u32*)src, (u32*)dst, c->size);
	if (c->chain) {
		GX_RequestDma((u32*)(dst + c->chain_src_off), (u32*)out, c->chain_size);
	}
	gspWaitForDMA();
	// A second DMA's event may come before the wait takes the first: time for both.
	svcSleepThread(50 * 1000 * 1000);
	GSPGPU_InvalidateDataCache(dst - GUARD, c->size + 2 * GUARD);
	int got = 0, expected = 0, bad;
	if (c->chain) {
		GSPGPU_InvalidateDataCache(out - GUARD, c->chain_size + 2 * GUARD);
		bad = check(out, want + c->chain_src_off, c->chain_size, &got, &expected);
		if (bad >= c->chain_size + GUARD) {
			bad = check(dst, want, c->size, &got, &expected);
			if (bad < c->size + GUARD) {
				printf("\x1b[%d;1H%d %-24s BAD 1st @%d %02x!=%02x\x1b[K", 2 + k, k, c->name, bad,
				       got, expected);
				return 0;
			}
			printf("\x1b[%d;1H%d %-24s OK\x1b[K", 2 + k, k, c->name);
			return 1;
		}
		printf("\x1b[%d;1H%d %-24s BAD @%d %02x!=%02x\x1b[K", 2 + k, k, c->name, bad, got,
		       expected);
		return 0;
	}
	bad = check(dst, want, c->size, &got, &expected);
	if (bad < c->size + GUARD) {
		printf("\x1b[%d;1H%d %-24s BAD @%d %02x!=%02x\x1b[K", 2 + k, k, c->name, bad, got,
		       expected);
		return 0;
	}
	printf("\x1b[%d;1H%d %-24s OK\x1b[K", 2 + k, k, c->name);
	return 1;
}

int main() {
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	lin_src = linearAlloc(MAXSIZE + 64);
	lin_dst = linearAlloc(MAXSIZE + 64);
	lin_out = linearAlloc(MAXSIZE + 64);
	vram_a = vramAlloc(MAXSIZE + 64);
	vram_b = vramAlloc(MAXSIZE + 64);
	printf("\x1b[1;1Hdma_align_test\x1b[K");
	if (!lin_src || !lin_dst || !lin_out || !vram_a || !vram_b) {
		printf("\x1b[2;1Hallocation failed\x1b[K");
	}
	u32 round = 0;
	u64 next = 0;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		if (osGetTime() >= next && lin_src && lin_dst && lin_out && vram_a && vram_b) {
			round++;
			int ok = 0;
			for (unsigned k = 0; k < CASES; k++) {
				ok += run_case(&cases[k], k, (u8)(round * 13 + k));
			}
			if (ok == (int)CASES) {
				printf("\x1b[%d;1Hround %lu: ALL OK\x1b[K", 3 + (int)CASES, (unsigned long)round);
			} else {
				printf("\x1b[%d;1Hround %lu: %d of %d FAILED\x1b[K", 3 + (int)CASES,
				       (unsigned long)round, (int)CASES - ok, (int)CASES);
			}
			next = osGetTime() + 2000;
		}
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	gfxExit();
	return 0;
}
