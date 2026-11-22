// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// tiny_test: 2D textures with a side under 8 (which sgxd decodes on the CPU with upstream's
// LookupTexture), in every format but ETC1, drawn over a 64x64 RGBA8 target with
// one quad, point sampled, and the colour buffer hashed (FNV-1a): a build is checked against
// another build's lines. Each texture's buffer is whole 8x8 tiles, random bytes: a 4-wide
// texture's tile rows are no tiles apart (w / 8), so its rows from 8 on read tile 0 again. The
// last line says how many cases ran.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "tiny_shbin.h"

typedef struct {
	float x, y, z, u, v;
} vertex;

enum { W = 64, H = 64 };

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

static void DmaOut(const void* src, void* dst, u32 bytes) {
	memset(dst, 0xEE, bytes);
	GSPGPU_FlushDataCache(dst, bytes);
	GX_RequestDma((u32*)src, (u32*)dst, bytes);
	gspWaitForDMA();
	GSPGPU_InvalidateDataCache(dst, bytes);
}

static u32 Fnv(const u8* p, u32 n) {
	u32 h = 2166136261u;
	for (u32 i = 0; i < n; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static u32 seed;
static u8 Random(void) {
	seed = seed * 1664525u + 1013904223u;
	return (u8)(seed >> 24);
}

int main(void) {
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* t = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	u8* out = linearAlloc(W * H * 4);
	static const float quad[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1},
	                                 {1, -1, 1, 0},  {1, 1, 1, 1},  {-1, 1, 0, 1}};
	for (u32 k = 0; k < 6; k++) {
		v[k] = (vertex){quad[k][0], quad[k][1], -0.5f, quad[k][2], quad[k][3]};
	}
	GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
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
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

	static const struct {
		GPU_TEXCOLOR format;
		const char* name;
	} formats[] = {{GPU_RGBA8, "RGBA8"}, {GPU_RGB8, "RGB8"},   {GPU_RGBA5551, "RGB5A1"},
	               {GPU_RGB565, "RGB565"}, {GPU_RGBA4, "RGBA4"}, {GPU_LA8, "IA8"},
	               {GPU_HILO8, "RG8"},    {GPU_L8, "I8"},       {GPU_A8, "A8"},
	               {GPU_LA4, "IA4"},      {GPU_L4, "I4"},       {GPU_A4, "A4"}};
	static const u16 shapes[][2] = {{4, 4},  {4, 8}, {8, 4}, {4, 16}, {16, 4},
	                                {2, 8},  {8, 2}, {4, 32}, {32, 4}};
	char line[128];
	u32 cases = 0;
	for (u32 fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
		for (u32 s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
			const u16 w = shapes[s][0], h = shapes[s][1];
			// A buffer of whole 8x8 tiles, twice the texels or more; the unit's sides set after.
			C3D_Tex tex;
			if (!C3D_TexInit(&tex, w < 8 ? 8 : w, h < 8 ? 8 : h, formats[fi].format)) {
				snprintf(line, sizeof(line), "tiny_test %s %ux%u: no texture\n", formats[fi].name,
				         w, h);
				Log(line);
				continue;
			}
			seed = w * 977u + h * 31u + fi;
			u8* const data = tex.data;
			for (u32 i = 0; i < tex.size; i++) {
				data[i] = Random();
			}
			C3D_TexFlush(&tex);
			tex.width = w;
			tex.height = h;
			C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0, 0);
			C3D_FrameDrawOn(t);
			C3D_TexBind(0, &tex);
			C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
			C3D_FrameEnd(0);
			DmaOut(t->frameBuf.colorBuf, out, W * H * 4);
			snprintf(line, sizeof(line), "tiny_test %s %ux%u: %08lx\n", formats[fi].name, w, h,
			         (unsigned long)Fnv(out, W * H * 4));
			Log(line);
			printf("%s", line);
			C3D_TexDelete(&tex);
			cases++;
		}
	}
	snprintf(line, sizeof(line), "tiny_test: %lu cases done\n", (unsigned long)cases);
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
	linearFree(out);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	C3D_Fini();
	gfxExit();
	return 0;
}
