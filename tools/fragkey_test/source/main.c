// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// fragkey_test: an alpha-tested, alpha-blended quad (alpha 0 on its left edge to 1 on its right,
// the test GREATER 0.5: its left half's fragments are killed) drawn with depth write on, then a
// red quad behind it: the red must show where the test killed fragments (they wrote no depth)
// and stay hidden where they passed. The right half draws first in the frame (the red quad's
// state before it makes the fragment state dirty): a warm-up strip with depth write off (a
// no-kill program is valid there), then the quad with depth write on and nothing else changed:
// an emulator keying its fragment programs without the depth write reuses the no-kill program,
// and the killed half writes depth, hiding the red. The left half, the control, draws last with
// another TEV (the same colour) and depth write on: its own program. Expected: red in both
// halves' left quarters. Start quits.
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z; float r, g, b, a; } vertex;

// A quad as two triangles: x0..x1, y0..y1 at z, colour rgb, alpha a0 at x0 to a1 at x1.
static void Quad(vertex* v, float x0, float x1, float y0, float y1, float z, float r, float g,
                 float b, float a0, float a1) {
	const vertex q[6] = {
		{x0, y0, z, r, g, b, a0}, {x1, y0, z, r, g, b, a1}, {x1, y1, z, r, g, b, a1},
		{x0, y0, z, r, g, b, a0}, {x1, y1, z, r, g, b, a1}, {x0, y1, z, r, g, b, a0},
	};
	memcpy(v, q, sizeof(q));
}

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
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
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4);
	C3D_Mtx projection;
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);

	// The quads: left fresh (depth write on), right warm-up (depth write off, top strip), right
	// (depth write on), red behind (whole screen).
	vertex* vbo = linearAlloc(sizeof(vertex) * 6 * 4);
	Quad(vbo + 0, 20, 180, 40, 180, 0.5f, 1, 1, 1, 0, 1);
	Quad(vbo + 6, 220, 380, 190, 230, 0.5f, 1, 1, 1, 0, 1);
	Quad(vbo + 12, 220, 380, 40, 180, 0.5f, 1, 1, 1, 0, 1);
	Quad(vbo + 18, 0, 400, 0, 240, 0.75f, 1, 0, 0, 1, 1);
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo, sizeof(vertex), 2, 0x10);

	C3D_TexEnv* env = NULL;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x0000FFFF, 0);
		C3D_FrameDrawOn(target);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
		// The alpha-tested, blended quads: right warm-up (depth write off), right (depth write
		// on), then the left control under another TEV (primary colour times white).
		C3D_AlphaTest(true, GPU_GREATER, 0x80);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		// C3D_GetTexEnv marks the stage for upload: a change through an old pointer is not sent.
		env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_DrawArrays(GPU_TRIANGLES, 6, 6);
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
		C3D_DrawArrays(GPU_TRIANGLES, 12, 6);
		env = C3D_GetTexEnv(0);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_CONSTANT, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexEnvColor(env, 0xFFFFFFFF);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		// The red quad behind: no alpha test, no blend.
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
		C3D_ColorLogicOp(GPU_LOGICOP_COPY);
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_DrawArrays(GPU_TRIANGLES, 18, 6);
		C3D_FrameEnd(0);
	}
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
