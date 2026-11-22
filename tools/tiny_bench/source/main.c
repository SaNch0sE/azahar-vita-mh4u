// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// tiny_bench: the cost of untiling textures with a side under 8. Each frame draws N small quads
// over a 64x64 RGBA8 target, each sampling its own tiny texture (RGBA8, in linear FCRAM). In the
// "static" phase the textures stay as they are: after the first frame nothing is untiled. In the
// "rewritten" phase the CPU writes every texture's texels first, so each draw finds its texture
// written and untiles it again. Each phase runs SECONDS seconds and logs its frames and the time a
// frame; (rewritten - static) / N is one untile's cost on the frame's path. Shapes 4x4 and 4x64.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "tiny_shbin.h"

typedef struct {
	float x, y, z, u, v;
} vertex;

enum { W = 64, H = 64, N = 32, SECONDS = 8 };

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

static void Phase(const char* name, C3D_Tex* tex, u32 w, u32 h, bool rewrite,
                  C3D_RenderTarget* t, char* line, int n) {
	const u64 ticks_per_second = SYSCLOCK_ARM11;
	const u64 start = svcGetSystemTick();
	u32 frames = 0, seed = 1;
	while (svcGetSystemTick() - start < SECONDS * ticks_per_second) {
		if (rewrite) {
			for (u32 k = 0; k < N; k++) {
				u32* const texels = tex[k].data;
				for (u32 i = 0; i < w * h; i++) {
					seed = seed * 1664525u + 1013904223u;
					texels[i] = seed | 0xff;
				}
				GSPGPU_FlushDataCache(texels, tex[k].size);
			}
		}
		// Not SYNCDRAW: that waits for citro3d's 60 fps frame counter; 0 waits for the GPU alone.
		C3D_FrameBegin(0);
		C3D_FrameDrawOn(t);
		for (u32 k = 0; k < N; k++) {
			C3D_TexBind(0, &tex[k]);
			C3D_DrawArrays(GPU_TRIANGLES, 6 * k, 6);
		}
		C3D_FrameEnd(0);
		frames++;
	}
	const u64 us = (svcGetSystemTick() - start) * 1000000 / ticks_per_second;
	snprintf(line, n, "tiny_bench %ux%u x%u %s: %lu frames in %llu ms, %llu us a frame\n",
	         (unsigned)w, (unsigned)h, (unsigned)N, name, (unsigned long)frames, us / 1000,
	         frames ? us / frames : 0);
	Log(line);
	printf("%s", line);
}

int main(void) {
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* t = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	// N quads in a row of 8 by 4, each 1/8 of the target wide.
	vertex* v = linearAlloc(N * 6 * sizeof(vertex));
	static const float quad[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
	for (u32 k = 0; k < N; k++) {
		const float x0 = -1 + (k % 8) * 0.25f, y0 = -1 + (k / 8) * 0.5f;
		for (u32 c = 0; c < 6; c++) {
			v[6 * k + c] = (vertex){x0 + quad[c][0] * 0.25f, y0 + quad[c][1] * 0.5f, -0.5f,
			                        quad[c][0], quad[c][1]};
		}
	}
	GSPGPU_FlushDataCache(v, N * 6 * sizeof(vertex));
	DVLB_s* dvlb = DVLB_ParseFile((u32*)tiny_shbin, tiny_shbin_size);
	static shaderProgram_s prog;
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	C3D_BindProgram(&prog);
	C3D_AttrInfo* a = C3D_GetAttrInfo();
	AttrInfo_Init(a);
	AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(a, 1, GPU_FLOAT, 2);
	C3D_BufInfo* b = C3D_GetBufInfo();
	BufInfo_Init(b);
	BufInfo_Add(b, v, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

	static const u16 shapes[][2] = {{4, 4}, {4, 64}};
	char line[160];
	static C3D_Tex tex[N];
	for (u32 s = 0; s < 2; s++) {
		const u16 w = shapes[s][0], h = shapes[s][1];
		for (u32 k = 0; k < N; k++) {
			// Whole 8x8 tiles of RGBA8; the unit's sides set after (C3D_TexInit takes 8 and up).
			if (!C3D_TexInit(&tex[k], 8, h < 8 ? 8 : h, GPU_RGBA8)) {
				Log("tiny_bench: no texture\n");
				return 1;
			}
			memset(tex[k].data, 0x80 + k, tex[k].size);
			C3D_TexFlush(&tex[k]);
			tex[k].width = w;
			tex[k].height = h;
			C3D_TexSetFilter(&tex[k], GPU_NEAREST, GPU_NEAREST);
		}
		Phase("static", tex, w, h, false, t, line, sizeof(line));
		Phase("rewritten", tex, w, h, true, t, line, sizeof(line));
		Phase("static again", tex, w, h, false, t, line, sizeof(line));
		for (u32 k = 0; k < N; k++) {
			C3D_TexDelete(&tex[k]);
		}
	}
	snprintf(line, sizeof(line), "tiny_bench: done\n");
	Log(line);
	printf("%s", line);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gspWaitForVBlank();
	}
	shaderProgramFree(&prog);
	DVLB_Free(dvlb);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	C3D_Fini();
	gfxExit();
	return 0;
}
