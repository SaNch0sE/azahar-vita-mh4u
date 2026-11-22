// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// colour_read_test: the CPU reading and writing a colour buffer the GPU drew, as games read back
// their render targets (screenshots, picking) and write into them: azahar's sgxd keeps a pass's
// colour in a picture of its own until something reads the buffer's bytes (lazy readback), and
// the guest's CPU and the HLE must get the copy first. A 240x400 RGBA8 target, each frame cleared
// to 0x202020FF and drawn with quad A (green) and quad B (blue), shown on the top screen. Every
// 60 frames, after the frame's drawing is done:
// - CPU read: the target's colour buffer read by the CPU at nine pixels (buffer x, y; tiled
//   addresses: 8x8 tiles, Morton inside), and its FNV-1a hash;
// - CPU write then DMA: a red 8x8 tile written by the CPU at (120, 100), then the buffer DMAed
//   into linear memory (GX_RequestDma, queued ahead of that frame's fill): the copy's pixels
//   there and beside it, read at the frame after.
// The bottom screen prints both. Start quits.
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

static u32 Fnv(const u8* p, u32 n) {
	u32 h = 2166136261u;
	for (u32 i = 0; i < n; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static u32 Word(const u8* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

#define POINTS 9
static const u32 px[POINTS] = {45, 80, 120, 160, 195, 120, 120, 120, 120};
static const u32 py[POINTS] = {100, 100, 100, 100, 100, 200, 230, 300, 370};

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_RenderTarget* target = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

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

	vertex* vbo = linearAlloc(sizeof(vertex) * 12);
	Quad(vbo + 0, 20, 180, 40, 200, 0, 1, 0);
	Quad(vbo + 6, 220, 380, 40, 200, 0, 0, 1);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	u8* const colour = (u8*)target->frameBuf.colorBuf;
	u8* out = linearAlloc(W * H * 4);
	printf("\x1b[1;1Hcolour_read_test\x1b[K");
	u32 frames = 0, reads = 0;
	bool read_due = false, dma_due = false;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		frames++;
		// SYNCDRAW: the last frame's queue (its fill, its list, its transfer, the DMA) has run,
		// the GPU's work with it: reads here see the frame drawn.
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (dma_due) {
			GSPGPU_InvalidateDataCache(out, W * H * 4);
			printf("\x1b[14;1HCPU write, DMA: hash %08lx\x1b[K", (unsigned long)Fnv(out, W * H * 4));
			printf("\x1b[15;1H 120,100 (red): %08lx\x1b[K", (unsigned long)Word(out + Tiled(120, 100) * 4));
			printf("\x1b[16;1H 127, 96 (red): %08lx\x1b[K", (unsigned long)Word(out + Tiled(127, 96) * 4));
			printf("\x1b[17;1H 128,100 (green): %08lx\x1b[K", (unsigned long)Word(out + Tiled(128, 100) * 4));
			printf("\x1b[18;1H  45,100 (green): %08lx\x1b[K", (unsigned long)Word(out + Tiled(45, 100) * 4));
			dma_due = false;
		}
		if (read_due) {
			// The last frame's colour, read by the CPU.
			GSPGPU_InvalidateDataCache(colour, W * H * 4);
			reads++;
			printf("\x1b[2;1Hread %lu (frame %lu)\x1b[K", (unsigned long)reads,
			       (unsigned long)frames - 1);
			printf("\x1b[3;1HCPU read: hash %08lx\x1b[K", (unsigned long)Fnv(colour, W * H * 4));
			for (int i = 0; i < POINTS; i++) {
				printf("\x1b[%d;1H %3lu,%3lu: %08lx\x1b[K", 4 + i, (unsigned long)px[i],
				       (unsigned long)py[i], (unsigned long)Word(colour + Tiled(px[i], py[i]) * 4));
			}
			// A red tile written by the CPU, then the buffer DMAed out, queued ahead of this
			// frame's fill; the copy is read at the next frame's start.
			for (u32 y = 96; y < 104; y++) {
				for (u32 x = 120; x < 128; x++) {
					u8* const p = colour + Tiled(x, y) * 4;
					p[0] = 0xFF; // A, B, G, R in memory: red
					p[1] = 0x00;
					p[2] = 0x00;
					p[3] = 0xFF;
				}
			}
			GSPGPU_FlushDataCache(colour, W * H * 4);
			memset(out, 0xEE, W * H * 4);
			GSPGPU_FlushDataCache(out, W * H * 4);
			GX_RequestDma((u32*)colour, (u32*)out, W * H * 4);
			read_due = false;
			dma_due = true;
		}
		C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x202020FF, 0);
		C3D_FrameDrawOn(target);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 12);
		C3D_FrameEnd(0);
		if (frames >= 60 && frames % 60 == 0) {
			read_due = true;
		}
	}
	linearFree(out);
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
