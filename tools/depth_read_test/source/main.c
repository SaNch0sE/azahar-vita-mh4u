// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// depth_read_test: depth buffers read back by GSP DMA (GX_RequestDma), D16 and
// D24S8 (with 2x MSAA too). Two render targets
// of 240x400, one D24S8, one D16, each frame cleared (depth far, stencil 0x80) and drawn the same:
// quad A with z from 0.2 at its top edge to 0.6 at its bottom, quad B with z from 0.1 at its left
// edge to 0.9 at its right, depth test LESS, stencil set to 0x5A where they draw. The top screen
// shows one target, D24S8 and D16 in turn every 240 frames (azahar's MSAA option takes the shown
// one: each gets its 2x turn). Every 60 frames both depth buffers are DMAed into linear memory; the
// bottom screen prints the shown buffer's FNV-1a hash and its values at nine pixels (buffer x, y;
// tiled addresses: 8x8 tiles, Morton inside), D24S8's block from reads with it shown, D16's below
// from reads with D16 shown. Start quits.
//
// The CPU reads both depth buffers itself before each DMA, as a game reading its depth buffer
// back would: azahar's sgxd keeps a pass's depth on the GPU and writes it into the buffer only
// when something reads it, the guest's CPU included (its pages closed until then). The CPU's
// hash must be the DMA's; quad A's depth moves between reads (0.2, 0.3, 0.4 at its top), so a
// read of the bytes an earlier read left differs.
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

// z at the corners (x0, y0), (x1, y0), (x1, y1), (x0, y1).
static void Quad(vertex* v, float x0, float x1, float y0, float y1, float za, float zb, float zc,
                 float zd, float r, float g, float b) {
	const vertex q[6] = {
		{x0, y0, za, r, g, b, 1}, {x1, y0, zb, r, g, b, 1}, {x1, y1, zc, r, g, b, 1},
		{x0, y0, za, r, g, b, 1}, {x1, y1, zc, r, g, b, 1}, {x0, y1, zd, r, g, b, 1},
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

// Quad A's column of the buffer at y 100 across its x, quad B's row at x 120, the clear between.
#define POINTS 9
static const u32 px[POINTS] = {45, 80, 120, 160, 195, 120, 120, 120, 120};
static const u32 py[POINTS] = {100, 100, 100, 100, 100, 200, 230, 300, 370};

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_RenderTarget* t24 = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTarget* t16 = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	C3D_RenderTarget* shown = t24;
	C3D_RenderTargetSetOutput(shown, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

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
	Quad(vbo + 0, 20, 180, 40, 200, 0.2f, 0.2f, 0.6f, 0.6f, 0, 1, 0);
	Quad(vbo + 6, 220, 380, 40, 200, 0.1f, 0.9f, 0.9f, 0.1f, 0, 0, 1);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DepthTest(true, GPU_LESS, GPU_WRITE_ALL);
	C3D_StencilTest(true, GPU_ALWAYS, 0x5A, 0xFF, 0xFF);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);

	u8* out24 = linearAlloc(W * H * 4);
	u8* out16 = linearAlloc(W * H * 2);
	printf("\x1b[1;1Hdepth_read_test\x1b[K");
	u32 frames = 0, reads = 0, cpu24 = 0, cpu16 = 0, cpu_z = 0, failed = 0;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		frames++;
		// The frame read below is the previous one, drawn with the target shown then.
		C3D_RenderTarget* const read_shown = shown;
		if (frames % 240 == 0) {
			shown = shown == t24 ? t16 : t24;
			C3D_RenderTargetSetOutput(shown, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
		}
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		// The previous frame's drawing is done (SYNCDRAW): its depth, before this frame's clear.
		const bool read = frames > 1 && frames % 60 == 0;
		if (read) {
			// The CPU's own read first: the bytes the GPU's last pass left in the buffers.
			const u8* const d24 = (const u8*)t24->frameBuf.depthBuf;
			const u8* const d16 = (const u8*)t16->frameBuf.depthBuf;
			GSPGPU_InvalidateDataCache((void*)d24, W * H * 4);
			GSPGPU_InvalidateDataCache((void*)d16, W * H * 2);
			cpu24 = Fnv(d24, W * H * 4);
			cpu16 = Fnv(d16, W * H * 2);
			const u8* const p = d24 + Tiled(45, 100) * 4;
			cpu_z = p[0] | p[1] << 8 | p[2] << 16;
			memset(out24, 0xEE, W * H * 4);
			memset(out16, 0xEE, W * H * 2);
			GSPGPU_FlushDataCache(out24, W * H * 4);
			GSPGPU_FlushDataCache(out16, W * H * 2);
			GX_RequestDma((u32*)t24->frameBuf.depthBuf, (u32*)out24, W * H * 4);
			GX_RequestDma((u32*)t16->frameBuf.depthBuf, (u32*)out16, W * H * 2);
		}
		C3D_RenderTargetClear(t24, C3D_CLEAR_ALL, 0x202020FF, 0x80FFFFFF);
		C3D_RenderTargetClear(t16, C3D_CLEAR_ALL, 0x202020FF, 0xFFFF);
		C3D_FrameDrawOn(t24);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 12);
		C3D_FrameDrawOn(t16);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 12);
		C3D_FrameEnd(0);
		if (read) {
			gspWaitForDMA();
			svcSleepThread(50 * 1000 * 1000);
			GSPGPU_InvalidateDataCache(out24, W * H * 4);
			GSPGPU_InvalidateDataCache(out16, W * H * 2);
			reads++;
			printf("\x1b[2;1Hread %lu (frame %lu)\x1b[K", (unsigned long)reads, (unsigned long)frames);
			if (read_shown == t24) {
				printf("\x1b[3;1HD24S8 shown, read %lu: hash %08lx\x1b[K", (unsigned long)reads,
				       (unsigned long)Fnv(out24, W * H * 4));
				for (int i = 0; i < POINTS; i++) {
					const u8* p = out24 + Tiled(px[i], py[i]) * 4;
					const u32 z = p[0] | p[1] << 8 | p[2] << 16;
					printf("\x1b[%d;1H %3lu,%3lu: z %06lx s %02x\x1b[K", 4 + i, (unsigned long)px[i],
					       (unsigned long)py[i], (unsigned long)z, p[3]);
				}
			} else {
				printf("\x1b[14;1HD16 shown, read %lu: hash %08lx\x1b[K", (unsigned long)reads,
				       (unsigned long)Fnv(out16, W * H * 2));
				for (int i = 0; i < POINTS; i++) {
					const u8* p = out16 + Tiled(px[i], py[i]) * 2;
					printf("\x1b[%d;1H %3lu,%3lu: z %04x\x1b[K", 15 + i, (unsigned long)px[i],
					       (unsigned long)py[i], p[0] | p[1] << 8);
				}
			}
			const u32 dma24 = Fnv(out24, W * H * 4), dma16 = Fnv(out16, W * H * 2);
			const bool ok = cpu24 == dma24 && cpu16 == dma16;
			failed += ok ? 0 : 1;
			printf("\x1b[25;1HCPU D24S8 %08lx DMA %08lx\x1b[K", (unsigned long)cpu24,
			       (unsigned long)dma24);
			printf("\x1b[26;1HCPU D16   %08lx DMA %08lx\x1b[K", (unsigned long)cpu16,
			       (unsigned long)dma16);
			printf("\x1b[27;1HCPU z at 45,100 %06lx: %s, %lu of %lu reads failed\x1b[K",
			       (unsigned long)cpu_z, ok ? "match" : "DIFFER", (unsigned long)failed,
			       (unsigned long)reads);
			// Quad A deeper or nearer for the next read's frames.
			const float z = 0.2f + 0.1f * (float)(reads % 3);
			Quad(vbo + 0, 20, 180, 40, 200, z, z, z + 0.4f, z + 0.4f, 0, 1, 0);
			GSPGPU_FlushDataCache(vbo, sizeof(vertex) * 12);
		}
	}
	linearFree(out24);
	linearFree(out16);
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
