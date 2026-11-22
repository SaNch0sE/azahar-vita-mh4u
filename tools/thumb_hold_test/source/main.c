// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// thumb_hold_test: the guest's Thumb loads and stores on pages the GPU holds, beside the bytes
// the GPU uses there, as games keep their own data on the first page of a render target or
// beside a vertex buffer. The emulator makes such an access past the hold when it misses the
// queued work's bytes (and a picture's rows); otherwise the access waits for the GPU's work.
// Three pages, each 4 KiB aligned in linear memory:
// - page A: a 240x400 RGBA8 colour buffer from +0x800, drawn and never shown (QUADS quads);
// - page B: the frame's vertex buffer from +0x800 (the frame's draws read it);
// - page C: the top screen's 240x400 RGBA8 colour buffer from +0x800 (one red quad, then the
//   display transfer).
// Each frame is one command list (no memory fill: it holds the pages from its submission): a
// grey quad and QUADS green ones into A, a red quad into C. While the GPU draws, thumb_forms
// (forms.s: every ARMv6K Thumb load and store form) runs ITER times on each page's +0x400, its
// bytes zeroed once at the start (the routine's accesses are the first on the page a frame),
// against one run on plain memory: the values loaded and the bytes left must match. Every 60
// frames, after a frame drawn whole: the pages' routine bytes, A's pixels in page A and further
// on (green) and C's (red). The bottom screen shows the counts and each page's routine time a
// frame; the lines go to the debug output too. After RUN frames the last line says ALL OK or
// how many checks failed and stays (Start quits).
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define W 240
#define H 400
#define QUADS 200
#define ITER 8
#define RUN 900
#define FORM_BYTES 0x110
#define OUTS 20
// The quads' green as Word reads it: A, B, G, R in memory.
#define Green 0x00FF00FFu
#define Red 0xFF0000FFu

void thumb_forms(u8* base, u32* out);

typedef struct { float x, y, z; float r, g, b, a; } vertex;

static void Quad(vertex* v, float x0, float x1, float y0, float y1, float r, float g, float b) {
	const vertex q[6] = {
		{x0, y0, 0.5f, r, g, b, 1}, {x1, y0, 0.5f, r, g, b, 1}, {x1, y1, 0.5f, r, g, b, 1},
		{x0, y0, 0.5f, r, g, b, 1}, {x1, y1, 0.5f, r, g, b, 1}, {x0, y1, 0.5f, r, g, b, 1},
	};
	memcpy(v, q, sizeof(q));
}

static u32 Tiled(u32 bx, u32 by) {
	u32 m = 0;
	for (u32 i = 0; i < 3; i++) {
		m |= ((bx >> i) & 1) << (2 * i) | ((by >> i) & 1) << (2 * i + 1);
	}
	return ((by / 8) * (W / 8) + bx / 8) * 64 + m;
}

static u32 Word(const u8* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

// The reference: thumb_forms on plain memory, never held.
static u8 ref_mem[FORM_BYTES + 8] __attribute__((aligned(8)));
static u32 ref_out[OUTS];

// thumb_forms on base (a held page's bytes, zeroed once at the start) against the reference: 0
// when the values loaded and the bytes left match.
static u32 Check(u8* base) {
	u32 out[OUTS];
	thumb_forms(base, out);
	return (memcmp(out, ref_out, sizeof(out)) != 0) + (memcmp(base, ref_mem, FORM_BYTES) != 0);
}

#define PAGES 3
static const char* const page_names[PAGES] = {"A (drawn)", "B (vertices)", "C (shown)"};

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);

	u8* pages[PAGES];
	pages[0] = linearMemAlign(0x800 + W * H * 4, 0x1000);
	pages[1] = linearMemAlign(0x800 + sizeof(vertex) * 18, 0x1000);
	pages[2] = linearMemAlign(0x800 + W * H * 4, 0x1000);
	for (int i = 0; i < PAGES; i++) {
		memset(pages[i], 0, 0x800);
	}
	u8* const drawn = pages[0] + 0x800;
	u8* const shown = pages[2] + 0x800;
	vertex* const vbo = (vertex*)(pages[1] + 0x800);
	Quad(vbo + 0, 0, 400, 0, 240, 0.25f, 0.25f, 0.25f);
	Quad(vbo + 6, 0, 400, 0, 240, 0, 1, 0);
	Quad(vbo + 12, 0, 400, 0, 240, 1, 0, 0);
	GSPGPU_FlushDataCache(pages[1], 0x1000);
	GSPGPU_FlushDataCache(pages[0], 0x800);
	GSPGPU_FlushDataCache(pages[2], 0x800);

	C3D_RenderTarget* target_a = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, -1);
	target_a->frameBuf.colorBuf = drawn;
	C3D_RenderTarget* target_c = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, -1);
	target_c->frameBuf.colorBuf = shown;
	C3D_RenderTargetSetOutput(target_c, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgram_s program;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);
	const int uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4);
	C3D_Mtx projection;
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	memset(ref_mem, 0, sizeof(ref_mem));
	thumb_forms(ref_mem, ref_out);

	char line[200];
	snprintf(line, sizeof(line), "thumb_hold_test: page A %08lx (drawn +0x800), B %08lx (vertices "
	         "+0x800), C %08lx (shown +0x800), %d quads a frame, %d runs a page",
	         (unsigned long)osConvertVirtToPhys(pages[0]), (unsigned long)osConvertVirtToPhys(pages[1]),
	         (unsigned long)osConvertVirtToPhys(pages[2]), QUADS, ITER);
	Log(line);
	printf("\x1b[1;1Hthumb_hold_test\x1b[K");
	u32 frames = 0, form_fails = 0, pixel_fails = 0, ticks_frames = 0;
	u64 ticks[PAGES] = {0};
	bool check_due = false;
	while (aptMainLoop() && frames < RUN) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		frames++;
		C3D_FrameBegin(check_due ? C3D_FRAME_SYNCDRAW : 0);
		if (check_due) {
			// The last frame drawn whole: the GPU's pixels in the pages and past them, and the
			// routine's bytes as the last runs left them.
			GSPGPU_InvalidateDataCache(pages[0], 0x800 + W * H * 4);
			GSPGPU_InvalidateDataCache(pages[2], 0x800 + W * H * 4);
			const u32 a_near = Word(drawn + Tiled(4, 4) * 4), a_far = Word(drawn + Tiled(200, 300) * 4);
			const u32 c_near = Word(shown + Tiled(4, 4) * 4), c_far = Word(shown + Tiled(200, 300) * 4);
			u32 bad = (a_near != Green) + (a_far != Green) + (c_near != Red) + (c_far != Red);
			for (int i = 0; i < PAGES; i++) {
				bad += memcmp(pages[i] + 0x400, ref_mem, FORM_BYTES) != 0;
			}
			pixel_fails += bad;
			const u32 n = ticks_frames ? ticks_frames : 1;
			snprintf(line, sizeof(line), "thumb_hold_test: frame %lu: pixels A %08lx %08lx, C %08lx "
			         "%08lx; form checks failed %lu, pixel and byte checks %lu; a frame's runs: A %lu "
			         "us, B %lu us, C %lu us", (unsigned long)frames, (unsigned long)a_near,
			         (unsigned long)a_far, (unsigned long)c_near, (unsigned long)c_far,
			         (unsigned long)form_fails, (unsigned long)pixel_fails,
			         (unsigned long)(ticks[0] / n / 268), (unsigned long)(ticks[1] / n / 268),
			         (unsigned long)(ticks[2] / n / 268));
			Log(line);
			printf("\x1b[3;1Hframe %lu\x1b[K", (unsigned long)frames);
			printf("\x1b[4;1HA %08lx %08lx C %08lx %08lx\x1b[K", (unsigned long)a_near,
			       (unsigned long)a_far, (unsigned long)c_near, (unsigned long)c_far);
			printf("\x1b[5;1Hform checks failed %lu\x1b[K", (unsigned long)form_fails);
			printf("\x1b[6;1Hpixel/byte checks failed %lu\x1b[K", (unsigned long)pixel_fails);
			for (int i = 0; i < PAGES; i++) {
				printf("\x1b[%d;1Hpage %s: %lu us a frame\x1b[K", 7 + i, page_names[i],
				       (unsigned long)(ticks[i] / n / 268));
				ticks[i] = 0;
			}
			ticks_frames = 0;
			check_due = false;
		}
		C3D_FrameDrawOn(target_a);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		for (int i = 0; i < QUADS; i++) {
			C3D_DrawArrays(GPU_TRIANGLES, 6, 6);
		}
		C3D_FrameDrawOn(target_c);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 12, 6);
		C3D_FrameEnd(0);
		// The frame's list is queued: the pages held while the GPU draws.
		for (int p = 0; p < PAGES; p++) {
			const u64 t0 = svcGetSystemTick();
			for (int i = 0; i < ITER; i++) {
				form_fails += Check(pages[p] + 0x400);
			}
			ticks[p] += svcGetSystemTick() - t0;
		}
		ticks_frames++;
		if (frames % 60 == 0) {
			check_due = true;
		}
	}
	const u32 failed = form_fails + pixel_fails;
	if (failed) {
		snprintf(line, sizeof(line), "thumb_hold_test: %lu FAILED (%lu frames)",
		         (unsigned long)failed, (unsigned long)frames);
	} else {
		snprintf(line, sizeof(line), "thumb_hold_test: ALL OK (%lu frames)", (unsigned long)frames);
	}
	Log(line);
	printf("\x1b[11;1H%s\x1b[K", line);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gspWaitForVBlank();
	}
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
