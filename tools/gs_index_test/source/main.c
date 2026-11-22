// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// gs_index_test: float uniforms read with an address register in a fixed-primitive geometry
// program.
//
// Each case draws two primitives of four vertices (GPU_GEOMETRY_PRIM; position and colour, c0..c7
// in the geometry program) into a 64x64 RGBA8 target cleared to zero, with additive blending. Each
// invocation covers the whole target with two triangles of one colour, so every pixel ends as the
// two invocations' colours summed. The first primitive's vertices have red 0x10, 0x20, 0x40 and
// 0x80, the second's green.
//  A  the four vertices' colours summed in a LOOP at rows[aL + 1]; the corners at rows[0],
//     rows[a0.x], rows[a0.y], rows[a0.x + 4]: F0 F0 00 FF
//  B  pal[a0.x] (c10, a shared uniform: blue 0x20) plus pal[a0.y] with a0.y = -7 (c1, the first
//     vertex's colour, a row reached from outside the rows): 10 10 40 FF
//  C  A with a temporary read before it is written (the invocations in order): as A
// The colour buffer is DMAed out and every pixel compared, each channel within 1. The last line
// says ALL OK or how many cases failed.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "gsa_shbin.h"
#include "gsb_shbin.h"
#include "gsc_shbin.h"

typedef struct {
	float x, y, z, r, g, b, a;
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

static u32 Word(const u8* p, u32 i) {
	p += 4 * i;
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static bool Near(u32 got, u32 want) {
	for (int s = 0; s < 32; s += 8) {
		const int a = (got >> s) & 0xFF, b = (want >> s) & 0xFF;
		if (a - b > 1 || b - a > 1) {
			return false;
		}
	}
	return true;
}

typedef struct {
	const char* name;
	const u8* shbin;
	u32 size;
	u32 want; // R G B A, high to low
} Case;

// One case's draw, then every pixel against its colour; the mismatches.
// Each case's program and its DVLB stay until the end: citro3d uploads a program's code only when
// its DVLP is another one's, and a DVLB parsed after one freed lands at its address.
static u32 Run(const Case* c, shaderProgram_s* prog, C3D_RenderTarget* t, vertex* v, u8* out,
               char* line, int n) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0, 0);
	C3D_FrameDrawOn(t);
	C3D_BindProgram(prog);
	const int pal = shaderInstanceGetUniformLocation(prog->geometryShader, "pal");
	if (pal >= 0) {
		const float k = 1.0f / 255.0f;
		C3D_FVUnifSet(GPU_GEOMETRY_SHADER, pal + 0, 0, 0, 0x08 * k, 0);
		C3D_FVUnifSet(GPU_GEOMETRY_SHADER, pal + 1, 0, 0, 0x04 * k, 0);
		C3D_FVUnifSet(GPU_GEOMETRY_SHADER, pal + 2, 0, 0, 0x20 * k, 0);
		C3D_FVUnifSet(GPU_GEOMETRY_SHADER, pal + 3, 0, 0, 0x02 * k, 0);
	}
	C3D_AttrInfo* a = C3D_GetAttrInfo();
	AttrInfo_Init(a);
	AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(a, 1, GPU_FLOAT, 4);
	C3D_BufInfo* b = C3D_GetBufInfo();
	BufInfo_Init(b);
	BufInfo_Add(b, v, sizeof(vertex), 2, 0x10);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 8);
	C3D_FrameEnd(0);

	DmaOut(t->frameBuf.colorBuf, out, W * H * 4);
	u32 bad = 0, first = 0, first_got = 0;
	for (u32 i = 0; i < W * H; i++) {
		const u32 got = Word(out, i);
		if (!Near(got, c->want) && bad++ == 0) {
			first = i;
			first_got = got;
		}
	}
	if (bad == 0) {
		snprintf(line, n, "gs_index_test %s: ok (%08lx)\n", c->name, (unsigned long)c->want);
	} else {
		snprintf(line, n, "gs_index_test %s: %lu of %u pixels bad (first %lu: %08lx, want %08lx)\n",
		         c->name, (unsigned long)bad, W * H, (unsigned long)first,
		         (unsigned long)first_got, (unsigned long)c->want);
	}
	return bad;
}

int main(void) {
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* t = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	vertex* v = linearAlloc(8 * sizeof(vertex));
	u8* out = linearAlloc(W * H * 4);
	static const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	for (int p = 0; p < 2; p++) {
		for (int k = 0; k < 4; k++) {
			const float c = (float)(0x10 << k) / 255.0f;
			v[4 * p + k] = (vertex){corners[k][0], corners[k][1], -0.5f, p == 0 ? c : 0,
			                        p == 1 ? c : 0, 0, 0};
		}
	}
	const Case cases[] = {
		{"A (aL in a LOOP, a0)", gsa_shbin, gsa_shbin_size, 0xF0F000FFu},
		{"B (a0 past the rows, into them)", gsb_shbin, gsb_shbin_size, 0x101040FFu},
		{"C (A in order)", gsc_shbin, gsc_shbin_size, 0xF0F000FFu},
	};
	enum { CASES = sizeof(cases) / sizeof(cases[0]) };
	DVLB_s* dvlb[CASES];
	static shaderProgram_s prog[CASES];
	for (u32 k = 0; k < CASES; k++) {
		dvlb[k] = DVLB_ParseFile((u32*)cases[k].shbin, cases[k].size);
		shaderProgramInit(&prog[k]);
		shaderProgramSetVsh(&prog[k], &dvlb[k]->DVLE[0]);
		// Stride 0: the vertex program's outputs, two.
		shaderProgramSetGsh(&prog[k], &dvlb[k]->DVLE[1], 0);
	}
	u32 failed = 0;
	char line[160];
	for (u32 k = 0; k < CASES; k++) {
		if (Run(&cases[k], &prog[k], t, v, out, line, sizeof(line)) != 0) {
			failed++;
		}
		Log(line);
		printf("%s", line);
	}
	if (failed == 0) {
		snprintf(line, sizeof(line), "gs_index_test: ALL OK\n");
	} else {
		snprintf(line, sizeof(line), "gs_index_test: %lu of %u cases FAILED\n",
		         (unsigned long)failed, (unsigned)CASES);
	}
	Log(line);
	printf("%s", line);

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gspWaitForVBlank();
	}
	for (u32 k = 0; k < CASES; k++) {
		shaderProgramFree(&prog[k]);
		DVLB_Free(dvlb[k]);
	}
	linearFree(out);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	C3D_Fini();
	gfxExit();
	return 0;
}
