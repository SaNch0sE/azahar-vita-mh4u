// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// applet_test: the framebuffers across a library applet. libctru's applet launch has GSP save
// VRAM's system area and the framebuffers before it (SaveVramSysArea) and restore them after
// (RestoreVramSysArea); the software keyboard here is the emulator's, which answers at once.
// Three phases, each with its own framebuffers (BGR8, single buffered):
//   linear: framebuffers in linear memory, both filled by the CPU with a pattern, shown;
//   vram: the same with the framebuffers in VRAM;
//   gpu: linear framebuffers, the top one written by a display transfer (RGBA8 to RGB8) from a
//        pattern in linear memory, so that what shows it is the GPU's copy; its bytes hashed
//        before the applet and after.
// After the applet, each framebuffer's bytes are compared with what was there before. The bottom
// screen prints each phase's result and a last line, ALL OK or the failed count, for 3 s (Start
// quits sooner); the lines go to the debug output too.
#include <3ds.h>
#include <stdio.h>
#include <string.h>

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

static u8 Pattern(u32 i, u32 seed) {
	return (u8)((i * 131u + seed * 77u) ^ (i >> 9));
}

static void Fill(u8* p, u32 bytes, u32 seed) {
	for (u32 i = 0; i < bytes; i++) {
		p[i] = Pattern(i, seed);
	}
}

static u32 Differ(const u8* p, u32 bytes, u32 seed) {
	u32 n = 0;
	for (u32 i = 0; i < bytes; i++) {
		n += p[i] != Pattern(i, seed);
	}
	return n;
}

static u32 Hash(const u8* p, u32 bytes) {
	u32 h = 2166136261u;
	for (u32 i = 0; i < bytes; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static void Wait(int vblanks) {
	for (int i = 0; i < vblanks; i++) {
		gspWaitForVBlank();
	}
}

// The software keyboard, which the emulator answers at once: its button and text.
static SwkbdButton Applet(char* text, size_t size) {
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 1, 32);
	text[0] = 0;
	return swkbdInputText(&kb, text, size);
}

// Returns 0 when the framebuffers came back as they were.
static int Phase(const char* name, bool vram, bool gpu, char* line, size_t line_size) {
	gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, vram);
	gfxSetDoubleBuffering(GFX_TOP, false);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	u16 tw, th, bw, bh;
	u8* top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &tw, &th);
	u8* bottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);
	const u32 top_bytes = tw * th * 3, bottom_bytes = bw * bh * 3;
	Fill(bottom, bottom_bytes, 2);
	u32 top_hash = 0;
	if (gpu) {
		// tw x th is the framebuffer's 240 x 400 as memory lays it out.
		u8* source = linearAlloc(tw * th * 4);
		Fill(source, tw * th * 4, 3);
		GSPGPU_FlushDataCache(source, tw * th * 4);
		GX_DisplayTransfer((u32*)source, GX_BUFFER_DIM(tw, th), (u32*)top, GX_BUFFER_DIM(tw, th),
		                   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
		                       GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
		gspWaitForPPF();
		linearFree(source);
	} else {
		Fill(top, top_bytes, 1);
	}
	gfxFlushBuffers();
	gfxSwapBuffers();
	Wait(10);
	if (gpu) {
		GSPGPU_InvalidateDataCache(top, top_bytes);
		top_hash = Hash(top, top_bytes);
	}
	char text[64];
	const SwkbdButton button = Applet(text, sizeof(text));
	Wait(4);
	GSPGPU_InvalidateDataCache(top, top_bytes);
	GSPGPU_InvalidateDataCache(bottom, bottom_bytes);
	const u32 top_now = gpu ? Hash(top, top_bytes) : 0;
	const u32 top_differ = gpu ? (top_now != top_hash) : Differ(top, top_bytes, 1);
	const u32 bottom_differ = Differ(bottom, bottom_bytes, 2);
	if (gpu) {
		snprintf(line, line_size,
		         "applet_test %s: button %d text '%s'; top hash %08lx before, %08lx after; "
		         "bottom %lu of %lu bytes differ",
		         name, (int)button, text, top_hash, top_now, bottom_differ, bottom_bytes);
	} else {
		snprintf(line, line_size,
		         "applet_test %s: button %d text '%s'; top %lu of %lu bytes differ, bottom %lu of "
		         "%lu",
		         name, (int)button, text, top_differ, top_bytes, bottom_differ, bottom_bytes);
	}
	Log(line);
	gfxExit();
	return top_differ != 0 || bottom_differ != 0;
}

int main(void) {
	static char lines[3][200];
	int failed = 0;
	failed += Phase("linear", false, false, lines[0], sizeof(lines[0]));
	failed += Phase("vram", true, false, lines[1], sizeof(lines[1]));
	failed += Phase("gpu", false, true, lines[2], sizeof(lines[2]));
	char last[64];
	snprintf(last, sizeof(last), failed ? "applet_test: %d FAILED" : "applet_test: ALL OK", failed);
	Log(last);

	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	for (int i = 0; i < 3; i++) {
		printf("%s\n\n", lines[i]);
	}
	printf("%s\n", last);
	// Shown for 3 s (or until Start), then it quits.
	for (int i = 0; i < 180 && aptMainLoop(); i++) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gspWaitForVBlank();
	}
	gfxExit();
	return 0;
}
