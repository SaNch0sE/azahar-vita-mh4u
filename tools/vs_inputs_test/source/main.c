// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// vs_inputs_test: seven vertical strips over the top screen, each drawn by its own vertex program
// (p0-p6.v.pica) from the same four fixed attributes (va = (1, 2, 3), vb = (4, 5, 6),
// vc = (0.3, 0.4, 0.5), vd = (0.1, 0.05, 0.02)), each writing one value as the colour: va / 4,
// vb / 8, vc, vd * 5, va.yzx * vb.zxy / 16, dp3((12, 12, 5), vd) / 2 and usse_cross_test's whole
// expression. Expected, left to right (RGB, 0-255): (64, 128, 191), (128, 159, 191),
// (77, 102, 128), (128, 64, 26), (191, 191, 80), (242, 0, 0), (242, 128, 255). The bottom screen
// prints the frame count and, once a second, each strip's bytes as the displayed framebuffer holds
// them. Start quits.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "p0_shbin.h"
#include "p1_shbin.h"
#include "p2_shbin.h"
#include "p3_shbin.h"
#include "p4_shbin.h"
#include "p5_shbin.h"
#include "p6_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define STRIPS 7

typedef struct { float x, y, z; } vertex;

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	const u8* const bins[STRIPS] = {p0_shbin, p1_shbin, p2_shbin, p3_shbin, p4_shbin, p5_shbin, p6_shbin};
	const u32 sizes[STRIPS] = {p0_shbin_size, p1_shbin_size, p2_shbin_size, p3_shbin_size,
	                           p4_shbin_size, p5_shbin_size, p6_shbin_size};
	DVLB_s* dvlb[STRIPS];
	shaderProgram_s program[STRIPS];
	int projection_at[STRIPS];
	for (int i = 0; i < STRIPS; i++) {
		dvlb[i] = DVLB_ParseFile((u32*)bins[i], sizes[i]);
		shaderProgramInit(&program[i]);
		shaderProgramSetVsh(&program[i], &dvlb[i]->DVLE[0]);
		projection_at[i] = shaderInstanceGetUniformLocation(program[i].vertexShader, "projection");
	}
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
	AttrInfo_AddFixed(attrInfo, 1);
	AttrInfo_AddFixed(attrInfo, 2);
	AttrInfo_AddFixed(attrInfo, 3);
	AttrInfo_AddFixed(attrInfo, 4);
	C3D_FixedAttribSet(1, 1.0f, 2.0f, 3.0f, 0.0f);
	C3D_FixedAttribSet(2, 4.0f, 5.0f, 6.0f, 0.0f);
	C3D_FixedAttribSet(3, 0.3f, 0.4f, 0.5f, 0.0f);
	C3D_FixedAttribSet(4, 0.1f, 0.05f, 0.02f, 0.0f);
	C3D_Mtx projection;
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);
	vertex* vbo = linearAlloc(sizeof(vertex) * 6 * STRIPS);
	for (int i = 0; i < STRIPS; i++) {
		const float x0 = 400.0f * i / STRIPS, x1 = 400.0f * (i + 1) / STRIPS;
		const vertex q[6] = {
			{x0, 0, 0.5f}, {x1, 0, 0.5f}, {x1, 240, 0.5f}, {x0, 0, 0.5f}, {x1, 240, 0.5f}, {x0, 240, 0.5f},
		};
		memcpy(vbo + 6 * i, q, sizeof(q));
	}
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 1, 0x0);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	u32 frames = 0;
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		printf("\x1b[1;1Hvs_inputs_test frame %lu\x1b[K", (unsigned long)++frames);
		if (frames % 60 == 0) {
			// The displayed top framebuffer's bytes (RGB8: B, G, R; 240 rows of the screen's columns,
			// bottom up) at each strip's middle, row 120: the GPU's output, past no capture.
			u16 w = 0, h = 0;
			const u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
			GSPGPU_InvalidateDataCache((void*)fb, 240 * 400 * 3);
			for (int i = 0; i < STRIPS; i++) {
				const int x = 400 * (2 * i + 1) / (2 * STRIPS), y = 120;
				const u8* px = fb + (x * 240 + (239 - y)) * 3;
				printf("\x1b[%d;1Hp%d: %3u %3u %3u\x1b[K", 3 + i, i, px[2], px[1], px[0]);
			}
		}
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(target);
		for (int i = 0; i < STRIPS; i++) {
			C3D_BindProgram(&program[i]);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, projection_at[i], &projection);
			C3D_DrawArrays(GPU_TRIANGLES, 6 * i, 6);
		}
		C3D_FrameEnd(0);
	}
	linearFree(vbo);
	for (int i = 0; i < STRIPS; i++) {
		shaderProgramFree(&program[i]);
		DVLB_Free(dvlb[i]);
	}
	C3D_Fini();
	gfxExit();
	return 0;
}
