// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// cube_test: cube maps sampled across each of their six faces, in ETC1, ETC1A4 and RGBA8, with
// one level and mipmapped, for the sgxd renderer's compressed cubes (an ETC1 CUBE, a split
// ETC1A4's A8 CUBE beside it).
//
// Each case fills every face's chain with random bytes (any bytes are valid ETC1 blocks) and
// draws one face a frame over a 64x64 RGBA8 target: a quad whose directions run along the face's
// two other axes to +-0.9, point sampled, the levels below the LOD minimum (minLevel) left out
// (the 128 and 512 faces are minified: a level past it may be read). The colour buffer is DMAed out and
// hashed (FNV-1a). A case's line has its six faces' hashes: a build is checked against another
// build's lines (the RGBA8 cases are the unchanged path). Two cases filter bilinearly. The last
// line says how many cases ran.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "cube_shbin.h"

typedef struct {
	float x, y, z, u, v, w;
} vertex;

enum { W = 64, H = 64 };

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

// bytes from src into dst by DMA (queued behind the frame's draws).
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

typedef struct {
	const char* name;
	GPU_TEXCOLOR format;
	u16 side;
	u8 max_level; // the chain's last level
	u8 level;     // the level read (minLevel)
	bool linear;
} Case;

static u32 seed;
static u8 Random(void) {
	seed = seed * 1664525u + 1013904223u;
	return (u8)(seed >> 24);
}

// Face f's quad: the major axis f / 2 at +-1, the next two axes from -0.9 to 0.9 across x and y.
static void FaceQuad(vertex* v, u32 f) {
	const u32 a = f / 2, ua = (a + 1) % 3, va = (a + 2) % 3;
	static const float corners[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
	for (u32 k = 0; k < 6; k++) {
		float d[3];
		d[a] = (f & 1) ? -1.0f : 1.0f;
		d[ua] = 0.9f * corners[k][0];
		d[va] = 0.9f * corners[k][1];
		v[k] = (vertex){corners[k][0], corners[k][1], -0.5f, d[0], d[1], d[2]};
	}
}

static void Run(const Case* c, C3D_RenderTarget* t, vertex* v, u8* out, char* line, int n) {
	C3D_Tex tex;
	C3D_TexCube cube;
	if (!C3D_TexInitWithParams(&tex, &cube,
	                           (C3D_TexInitParams){c->side, c->side, c->max_level, c->format,
	                                               GPU_TEX_CUBE_MAP, false})) {
		snprintf(line, n, "cube_test %s: no texture\n", c->name);
		return;
	}
	seed = c->side * 131u + c->max_level * 7u + c->format;
	for (u32 f = 0; f < 6; f++) {
		u32 size = 0;
		u8* data = C3D_TexCubeGetImagePtr(&tex, (GPU_TEXFACE)f, -1, &size);
		for (u32 i = 0; i < size; i++) {
			data[i] = Random();
		}
	}
	C3D_TexFlush(&tex);
	const GPU_TEXTURE_FILTER_PARAM filter = c->linear ? GPU_LINEAR : GPU_NEAREST;
	C3D_TexSetFilter(&tex, filter, filter);
	C3D_TexSetFilterMipmap(&tex, GPU_NEAREST);
	C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	tex.minLevel = c->level;
	tex.maxLevel = c->max_level;
	int at = snprintf(line, n, "cube_test %s:", c->name);
	for (u32 f = 0; f < 6; f++) {
		FaceQuad(v, f);
		GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0, 0);
		C3D_FrameDrawOn(t);
		C3D_TexBind(0, &tex);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		C3D_FrameEnd(0);
		DmaOut(t->frameBuf.colorBuf, out, W * H * 4);
		at += snprintf(line + at, n - at, " %08lx", (unsigned long)Fnv(out, W * H * 4));
	}
	snprintf(line + at, n - at, "\n");
	C3D_TexDelete(&tex);
}

int main(void) {
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* t = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	u8* out = linearAlloc(W * H * 4);
	DVLB_s* dvlb = DVLB_ParseFile((u32*)cube_shbin, cube_shbin_size);
	static shaderProgram_s prog;
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	C3D_BindProgram(&prog);
	C3D_AttrInfo* a = C3D_GetAttrInfo();
	AttrInfo_Init(a);
	AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(a, 1, GPU_FLOAT, 3);
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

	// The sides and chains: no mipmaps, two levels, a long chain; the FE Echoes crash's 512 cube.
	static const struct {
		u16 side;
		u8 max_level;
		u8 levels_read[3];
		u8 reads;
	} shapes[] = {
		{16, 0, {0}, 1},        {16, 1, {0, 1}, 2},       {64, 0, {0}, 1},
		{64, 3, {0, 1, 3}, 3},  {128, 4, {0, 2, 4}, 3},   {512, 0, {0}, 1},
	};
	static const struct {
		GPU_TEXCOLOR format;
		const char* name;
	} formats[] = {{GPU_ETC1, "ETC1"}, {GPU_ETC1A4, "ETC1A4"}, {GPU_RGBA8, "RGBA8"}};
	char name[48];
	char line[160];
	u32 cases = 0;
	for (u32 fi = 0; fi < 3; fi++) {
		for (u32 s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
			for (u32 r = 0; r < shapes[s].reads; r++) {
				snprintf(name, sizeof(name), "%s %ux%u/%u level %u", formats[fi].name,
				         shapes[s].side, shapes[s].side, shapes[s].max_level + 1,
				         shapes[s].levels_read[r]);
				const Case c = {name, formats[fi].format, shapes[s].side, shapes[s].max_level,
				                shapes[s].levels_read[r], false};
				Run(&c, t, v, out, line, sizeof(line));
				Log(line);
				printf("%s", line);
				cases++;
			}
		}
	}
	for (u32 fi = 0; fi < 2; fi++) {
		snprintf(name, sizeof(name), "%s 64x64/1 bilinear", formats[fi].name);
		const Case c = {name, formats[fi].format, 64, 0, 0, true};
		Run(&c, t, v, out, line, sizeof(line));
		Log(line);
		printf("%s", line);
		cases++;
	}
	snprintf(line, sizeof(line), "cube_test: %lu cases done\n", (unsigned long)cases);
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
