// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// narrow_depth_test: depth on a colour buffer of a width no sgxd pass takes: an indexed
// strip of 501 vertices into a 72x120 RGBA8 buffer with a 72x120 D24S8 depth buffer a fill had cleared.
//
// Each width, 72 and then 80 (the control: a width passes take), runs three passes on its own
// RGBA8 + D24S8 target, each quad an indexed triangle strip:
//  A  colour and depth cleared (fills: black, depth 0, stencil 0), then a red quad over
//     8..100 x 8..60 of the 120xW picture at depth 0.7, DCMP ALWAYS
//  B  no clear: a green quad over all of it at depth 0.4, GREATER: green where A left depth 0,
//     red where it left 0.7 (sgxd: the depth from its store)
//  C  the depth buffer written by DMA from a pattern (each pixel 0.3 or 0.9, stencil 0x5a),
//     then a blue quad over all of it at depth 0.6, GREATER: blue where the pattern is 0.3
//     (sgxd: the depth loaded from guest memory)
// After each pass the colour buffer and the depth buffer are DMAed out (sgxd: its depth written
// back) and checked pixel by pixel against each other: a pixel's colour and its depth lie at the
// same index in both (the same tiling, four bytes a pixel), so no check needs the tiling. Depth
// within 0x100 of 24 bits; the stencil exact. The last line says ALL OK or how many failed.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

typedef struct {
	float x, y, z, r, g, b, a;
} vertex;

enum { H = 120 };
#define BLACK 0x000000FFu
#define RED 0xFF0000FFu
#define GREEN 0x00FF00FFu
#define BLUE 0x0000FFFFu
// The depths (24 bits) of 0.7, 0.4, 0.6, 0.3 and 0.9.
#define D07 0xB33333u
#define D04 0x666666u
#define D06 0x999999u
#define D03 0x4CCCCCu
#define D09 0xE66666u
#define TOLERANCE 0x100u
#define PATTERN_STENCIL 0x5Au
// The red quad's pixels: 92 x 52.
#define RED_PIXELS (92u * 52u)

static DVLB_s* vs_dvlb;
static shaderProgram_s vs_prog;
static int vs_proj;

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

// A quad over [x0, x1] x [y0, y1] at z, as a strip's four vertices.
static void Strip(vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g,
                  float b) {
	const vertex q[4] = {
		{x0, y0, z, r, g, b, 1}, {x1, y0, z, r, g, b, 1},
		{x0, y1, z, r, g, b, 1}, {x1, y1, z, r, g, b, 1},
	};
	memcpy(v, q, sizeof(q));
}

// Strip k (indices 4k..4k+3) drawn with that depth test into t, in a frame of its own.
static void Pass(C3D_RenderTarget* t, const C3D_Mtx* proj, vertex* v, u16* idx, int k, bool clear,
                 GPU_TESTFUNC test) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	if (clear) {
		C3D_RenderTargetClear(t, C3D_CLEAR_ALL, BLACK, 0);
	}
	C3D_FrameDrawOn(t);
	C3D_BindProgram(&vs_prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, proj);
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
	// depth = 1 - z.
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, test, GPU_WRITE_ALL);
	C3D_DrawElements(GPU_TRIANGLE_STRIP, 4, C3D_UNSIGNED_SHORT, idx + 4 * k);
	C3D_FrameEnd(0);
}

// bytes from src into dst by DMA (queued behind the frame's draws).
static void Dma(const void* src, void* dst, u32 bytes, bool to_buffer) {
	if (!to_buffer) {
		memset(dst, 0xEE, bytes);
	}
	GSPGPU_FlushDataCache(to_buffer ? src : dst, bytes);
	GX_RequestDma((u32*)src, (u32*)dst, bytes);
	gspWaitForDMA();
	if (!to_buffer) {
		GSPGPU_InvalidateDataCache(dst, bytes);
	}
}

static u32 Word(const u8* p, u32 i) {
	p += 4 * i;
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static bool Near(u32 depth, u32 want) {
	const u32 d = depth & 0xFFFFFFu;
	return (d > want ? d - want : want - d) <= TOLERANCE;
}

// Whether pixel i of the pattern is 0.3 (else 0.9).
static bool PatternNear(u32 i) {
	return (i * 2654435761u) >> 31;
}

// A check's mismatches and its first one.
typedef struct {
	u32 bad, first, colour, depth;
} Check;

static void Mismatch(Check* c, u32 i, u32 colour, u32 depth) {
	if (c->bad++ == 0) {
		c->first = i;
		c->colour = colour;
		c->depth = depth;
	}
}

static int Report(char* out, int n, const char* name, const Check* c) {
	if (c->bad == 0) {
		return snprintf(out, n, " %s ok", name);
	}
	return snprintf(out, n, " %s %lu bad (first %lu: colour %08lx depth %08lx)", name,
	                (unsigned long)c->bad, (unsigned long)c->first, (unsigned long)c->colour,
	                (unsigned long)c->depth);
}

static u32 Run(u32 w, char* out, int out_size) {
	const u32 n = w * H, bytes = n * 4;
	C3D_RenderTarget* t = C3D_RenderTargetCreate(w, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	vertex* v = linearAlloc(12 * sizeof(vertex));
	u16* idx = linearAlloc(12 * sizeof(u16));
	// The picture is 120 x w, the buffer's rows across: the red quad inside it, the green and
	// blue ones over all of it.
	Strip(v, 8, 8, 100, 60, 0.3f, 1, 0, 0);
	Strip(v + 4, 0, 0, H, (float)w, 0.6f, 0, 1, 0);
	Strip(v + 8, 0, 0, H, (float)w, 0.4f, 0, 0, 1);
	for (u16 i = 0; i < 12; i++) {
		idx[i] = i;
	}
	GSPGPU_FlushDataCache(v, 12 * sizeof(vertex));
	GSPGPU_FlushDataCache(idx, 12 * sizeof(u16));
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, (float)H, 0.0f, (float)w, 0.0f, 1.0f, true);
	u8* colour[2] = {linearAlloc(bytes), linearAlloc(bytes)};
	u8* depth[2] = {linearAlloc(bytes), linearAlloc(bytes)};
	u8* pattern = linearAlloc(bytes);
	Check a = {0}, b = {0}, c = {0};
	u32 reds = 0, nears = 0;

	// A: cleared, the red quad at 0.7.
	Pass(t, &proj, v, idx, 0, true, GPU_ALWAYS);
	Dma(t->frameBuf.colorBuf, colour[0], bytes, false);
	Dma(t->frameBuf.depthBuf, depth[0], bytes, false);
	for (u32 i = 0; i < n; i++) {
		const u32 cl = Word(colour[0], i), d = Word(depth[0], i);
		const bool red = cl == RED;
		reds += red;
		if ((cl != RED && cl != BLACK) || !Near(d, red ? D07 : 0) || d >> 24 != 0) {
			Mismatch(&a, i, cl, d);
		}
	}
	if (reds != RED_PIXELS) {
		Mismatch(&a, n, reds, RED_PIXELS);
	}

	// B: the green quad at 0.4 where A left 0.
	Pass(t, &proj, v, idx, 1, false, GPU_GREATER);
	Dma(t->frameBuf.colorBuf, colour[1], bytes, false);
	Dma(t->frameBuf.depthBuf, depth[1], bytes, false);
	for (u32 i = 0; i < n; i++) {
		const bool red = Word(colour[0], i) == RED;
		const u32 cl = Word(colour[1], i), d = Word(depth[1], i);
		if (cl != (red ? RED : GREEN) || !Near(d, red ? D07 : D04) || d >> 24 != 0) {
			Mismatch(&b, i, cl, d);
		}
	}

	// C: the pattern into the depth buffer, the blue quad at 0.6 where it is 0.3.
	for (u32 i = 0; i < n; i++) {
		const bool near = PatternNear(i);
		nears += near;
		const u32 word = (near ? D03 : D09) | PATTERN_STENCIL << 24;
		memcpy(pattern + 4 * i, &word, 4);
	}
	Dma(pattern, t->frameBuf.depthBuf, bytes, true);
	Pass(t, &proj, v, idx, 2, false, GPU_GREATER);
	Dma(t->frameBuf.colorBuf, colour[0], bytes, false);
	Dma(t->frameBuf.depthBuf, depth[0], bytes, false);
	for (u32 i = 0; i < n; i++) {
		const bool near = PatternNear(i);
		const u32 before = Word(colour[1], i);
		const u32 cl = Word(colour[0], i), d = Word(depth[0], i);
		if (cl != (near ? BLUE : before) || !Near(d, near ? D06 : D09) ||
		    d >> 24 != PATTERN_STENCIL) {
			Mismatch(&c, i, cl, d);
		}
	}

	int at = snprintf(out, out_size, "%lux%u: red %lu, pattern 0.3 %lu of %lu;", (unsigned long)w, H,
	                  (unsigned long)reds, (unsigned long)nears, (unsigned long)n);
	at += Report(out + at, out_size - at, "A", &a);
	at += Report(out + at, out_size - at, "B", &b);
	Report(out + at, out_size - at, "C", &c);
	linearFree(pattern);
	for (int k = 0; k < 2; k++) {
		linearFree(depth[k]);
		linearFree(colour[k]);
	}
	linearFree(idx);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	return (a.bad != 0) + (b.bad != 0) + (c.bad != 0);
}

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	vs_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&vs_prog);
	shaderProgramSetVsh(&vs_prog, &vs_dvlb->DVLE[0]);
	vs_proj = shaderInstanceGetUniformLocation(vs_prog.vertexShader, "projection");

	static const u32 widths[2] = {72, 80};
	char line[300], detail[260];
	u32 failed = 0;
	printf("\x1b[1;1Hnarrow_depth_test\n");
	for (int k = 0; k < 2; k++) {
		snprintf(line, sizeof(line), "narrow_depth_test: %lu wide starts", (unsigned long)widths[k]);
		Log(line);
		detail[0] = 0;
		const u32 bad = Run(widths[k], detail, sizeof(detail));
		failed += bad;
		snprintf(line, sizeof(line), "narrow_depth_test: %s %s", bad ? "FAILED" : "ok", detail);
		Log(line);
		printf("%s\n\n", line + 19);
	}
	snprintf(line, sizeof(line), "narrow_depth_test: %s %lu", failed ? "FAILED" : "ALL OK",
	         (unsigned long)failed);
	Log(line);
	printf("%s\n", line + 19);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	shaderProgramFree(&vs_prog);
	DVLB_Free(vs_dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
