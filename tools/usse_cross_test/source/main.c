// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// usse_cross_test: one quad over the top screen whose vertex program (vshader.v.pica) computes a
// half cross product, a dp3 of another vector with itself and a dp3 with the product, and writes
// them as the colour: expected (242, 128, 255) on every pixel (upstream Azahar: 242, 127, 255),
// before and after the emitter's second tier takes the program over (after a hundred draws). The
// bottom screen prints the frame count. Start quits.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z; } vertex;

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
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
	static const vertex quad[6] = {
		{0, 0, 0.5f}, {400, 0, 0.5f}, {400, 240, 0.5f}, {0, 0, 0.5f}, {400, 240, 0.5f}, {0, 240, 0.5f},
	};
	vertex* vbo = linearAlloc(sizeof(quad));
	memcpy(vbo, quad, sizeof(quad));
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
		printf("\x1b[1;1Husse_cross_test frame %lu\x1b[K", (unsigned long)++frames);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(target);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		C3D_FrameEnd(0);
	}
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
