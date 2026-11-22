// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// colour_format_test: colour buffers in each PICA colour format drawn in two passes, the second
// blending over what the first stored, then read and written by the CPU and DMAed out. azahar's
// sgxd keeps a buffer's colour in a picture of its own until something reads its bytes; the
// picture must hold what the format holds (16-bit colour cut down at each store, no alpha in
// RGB8) for the second pass's blend, the reads and the copies to match the hardware's.
//
// Five 240x400 targets (RGBA8, RGB8, RGB565, RGB5A1, RGBA4), each frame:
// - pass 1: cleared to 0x202020FF, quad A green with alpha 0.25 (screen x 20..180), quad B blue
//   (220..380), both y 40..200, no blending;
// - pass 2 (after every target's pass 1): quad C (0.6, 0.3, 0.9, 0.5) blended source alpha over
//   x 100..300, y 100..220, and quad D white times the destination's alpha (DST_ALPHA, ZERO)
//   over x 30..70, y 100..140: RGB8 has no alpha, so D reads 1 there.
// The RGB565 target is shown on the top screen. Every 60 frames, at the next frame's start (its
// SYNCDRAW waits for the frame's queue, the GPU's work with it): each target's colour read by
// the CPU (its hash, and seven points: gray, D, green, green+C, gray+C, blue+C, blue, all at
// buffer x 120), then a red 8x8 tile written by the CPU at (120..127, 96..103) and the buffer
// DMAed into linear memory, queued ahead of that frame's fills (its hash, read at the frame
// after). The bottom screen prints them. Start quits.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define W 240
#define H 400
#define FORMATS 5

typedef struct { float x, y, z; float r, g, b, a; } vertex;

static void Quad(vertex* v, float x0, float x1, float y0, float y1, float r, float g, float b,
                 float a) {
	const vertex q[6] = {
		{x0, y0, 0.5f, r, g, b, a}, {x1, y0, 0.5f, r, g, b, a}, {x1, y1, 0.5f, r, g, b, a},
		{x0, y0, 0.5f, r, g, b, a}, {x1, y1, 0.5f, r, g, b, a}, {x0, y1, 0.5f, r, g, b, a},
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

static u32 Fnv(const u8* p, u32 n) {
	u32 h = 2166136261u;
	for (u32 i = 0; i < n; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static const GPU_COLORBUF formats[FORMATS] = {GPU_RB_RGBA8, GPU_RB_RGB8, GPU_RB_RGB565,
                                              GPU_RB_RGBA5551, GPU_RB_RGBA4};
static const char* const names[FORMATS] = {"RGBA8", "RGB8", "RGB565", "RGB5A1", "RGBA4"};
static const u32 bpps[FORMATS] = {4, 3, 2, 2, 2};
// Red in each format's memory order: RGBA8 bytes A, B, G, R; RGB8 B, G, R; the 16-bit ones as
// little-endian words (RGB565 R in 15:11; RGB5A1 R 15:11, A 0; RGBA4 R 15:12, A 3:0).
static const u8 reds[FORMATS][4] = {
	{0xff, 0x00, 0x00, 0xff}, {0x00, 0x00, 0xff}, {0x00, 0xf8}, {0x01, 0xf8}, {0x0f, 0xf0},
};

#define POINTS 7
static const u32 py[POINTS] = {10, 50, 82, 150, 200, 260, 340};

static u32 Pixel(const u8* p, u32 bpp) {
	u32 v = 0;
	for (u32 k = 0; k < bpp; k++) {
		v |= (u32)p[k] << (8 * k);
	}
	return v;
}

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_RenderTarget* targets[FORMATS];
	for (int f = 0; f < FORMATS; f++) {
		targets[f] = C3D_RenderTargetCreate(W, H, formats[f], -1);
	}
	C3D_RenderTargetSetOutput(targets[2], GFX_TOP, GFX_LEFT,
	                          GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
	                              GX_TRANSFER_RAW_COPY(0) |
	                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
	                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

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

	vertex* vbo = linearAlloc(sizeof(vertex) * 24);
	Quad(vbo + 0, 20, 180, 40, 200, 0, 1, 0, 0.25f);
	Quad(vbo + 6, 220, 380, 40, 200, 0, 0, 1, 1);
	Quad(vbo + 12, 100, 300, 100, 220, 0.6f, 0.3f, 0.9f, 0.5f);
	Quad(vbo + 18, 30, 70, 100, 140, 1, 1, 1, 1);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	u8* outs[FORMATS];
	for (int f = 0; f < FORMATS; f++) {
		outs[f] = linearAlloc(W * H * bpps[f]);
	}
	printf("\x1b[1;1Hcolour_format_test\x1b[K");
	u32 frames = 0, reads = 0;
	bool read_due = false, dma_due = false;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		frames++;
		// SYNCDRAW: the last frame's queue (its fills, its list, its transfer, the DMAs) has
		// run, the GPU's work with it: reads here see the frame drawn.
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (dma_due) {
			for (int f = 0; f < FORMATS; f++) {
				const u32 bytes = W * H * bpps[f];
				GSPGPU_InvalidateDataCache(outs[f], bytes);
				printf("\x1b[%d;22Hdma %08lx\x1b[K", 5 + 3 * f, (unsigned long)Fnv(outs[f], bytes));
			}
			dma_due = false;
		}
		if (read_due) {
			reads++;
			printf("\x1b[2;1Hread %lu (frame %lu)\x1b[K", (unsigned long)reads,
			       (unsigned long)frames - 1);
			printf("\x1b[3;1Hpoints at x 120, y %lu %lu %lu %lu / %lu %lu %lu\x1b[K",
			       (unsigned long)py[0], (unsigned long)py[1], (unsigned long)py[2],
			       (unsigned long)py[3], (unsigned long)py[4], (unsigned long)py[5],
			       (unsigned long)py[6]);
			for (int f = 0; f < FORMATS; f++) {
				u8* const colour = (u8*)targets[f]->frameBuf.colorBuf;
				const u32 bpp = bpps[f], bytes = W * H * bpp;
				GSPGPU_InvalidateDataCache(colour, bytes);
				const u32 read_hash = Fnv(colour, bytes);
				u32 v[POINTS];
				for (int i = 0; i < POINTS; i++) {
					v[i] = Pixel(colour + Tiled(120, py[i]) * bpp, bpp);
				}
				// The red tile, then the buffer DMAed out: queued ahead of this frame's
				// fills, read at the next frame's start.
				for (u32 y = 96; y < 104; y++) {
					for (u32 x = 120; x < 128; x++) {
						memcpy(colour + Tiled(x, y) * bpp, reds[f], bpp);
					}
				}
				GSPGPU_FlushDataCache(colour, bytes);
				memset(outs[f], 0xEE, bytes);
				GSPGPU_FlushDataCache(outs[f], bytes);
				GX_RequestDma((u32*)colour, (u32*)outs[f], bytes);
				const int row = 5 + 3 * f;
				const int digits = bpp * 2;
				printf("\x1b[%d;1H%-6s read %08lx\x1b[K", row, names[f], (unsigned long)read_hash);
				printf("\x1b[%d;1H %0*lx %0*lx %0*lx %0*lx\x1b[K", row + 1, digits,
				       (unsigned long)v[0], digits, (unsigned long)v[1], digits,
				       (unsigned long)v[2], digits, (unsigned long)v[3]);
				printf("\x1b[%d;1H %0*lx %0*lx %0*lx\x1b[K", row + 2, digits, (unsigned long)v[4],
				       digits, (unsigned long)v[5], digits, (unsigned long)v[6]);
			}
			read_due = false;
			dma_due = true;
		}
		for (int f = 0; f < FORMATS; f++) {
			C3D_RenderTargetClear(targets[f], C3D_CLEAR_ALL, 0x202020FF, 0);
		}
		// Pass 1 on every target, then pass 2 on every target: each pass 2 loads what its
		// pass 1 stored.
		for (int f = 0; f < FORMATS; f++) {
			C3D_FrameDrawOn(targets[f]);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			C3D_DrawArrays(GPU_TRIANGLES, 0, 12);
		}
		for (int f = 0; f < FORMATS; f++) {
			C3D_FrameDrawOn(targets[f]);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			C3D_DrawArrays(GPU_TRIANGLES, 12, 6);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ZERO, GPU_ONE,
			               GPU_ZERO);
			C3D_DrawArrays(GPU_TRIANGLES, 18, 6);
		}
		C3D_FrameEnd(0);
		if (frames >= 60 && frames % 60 == 0) {
			read_due = true;
		}
	}
	for (int f = 0; f < FORMATS; f++) {
		linearFree(outs[f]);
	}
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	for (int f = 0; f < FORMATS; f++) {
		C3D_RenderTargetDelete(targets[f]);
	}
	C3D_Fini();
	gfxExit();
	return 0;
}
