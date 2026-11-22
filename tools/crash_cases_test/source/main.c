// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// crash_cases_test: cases that stopped the emulator once, each run in turn; the last line says ALL
// OK or how many failed. CASES (a bit mask of the cases' numbers: make CASES=N SUFFIX=-name) builds a subset, for an emulator that stops on one case.
//
//  motion    the accelerometer and gyroscope enabled and read for 30 frames: azahar on the Vita
//            starts SceMotion's sampling at the first read; with a plugin sampling in the process
//            already (reVitaMotion) the start failed
//  d24s8 d16 d24 d24s8-w d24-w
//            a quad drawn into a 64x64 target with a depth buffer of that format (-w: W-buffered,
//            through a perspective projection), the depth buffer DMAed into memory and its first
//            pixel read: sgxd wrote back only D24S8 and D16 buffers of Z depths
//  gs-big    6000 triangles through a geometry program of six outputs: sgxd kept their stored
//            triangles and vertex outputs in a pass's 2 MiB of data, which they pass
//  d24s8-heap
//            a quad drawn into a 64x64 target whose D24S8 depth buffer is on the linear heap
//            (not at a page's start), the buffer's bytes then written by the CPU (a pattern)
//            and DMAed: the pattern, as the buffer is the guest's memory again; drawn into once
//            more and DMAed: the depth; drawn into a third time, its first two words written
//            and DMAed: those words.
//            sgxd kept the pass's depth as the newer and wrote it over the guest's bytes at the
//            DMA
//  tex-ones  a quad of a constant combiner colour drawn with all-ones in the three texture units'
//            parameter registers (unit 0's type field 7, no defined type; the units off): sgxd's
//            fragment emitter refused the state for the type though no stage reads the unit
//  depth-many
//            a quad drawn through a 240x400 viewport into a 256x512 target with each of 14
//            D24S8 depth buffers on the linear heap in turn, then each DMAed: the quad's depth
//            inside the viewport. sgxd keeps a pass's depth on the
//            GPU, 640 KiB a buffer of this size and as much again for a pass that stores part of
//            its target, out of 8 MiB, and freed none
//  gs-breakc a quad through a geometry program whose LOOP of eight rounds a BREAKC leaves at
//            round 2, 3 and never, the rounds run its green: sgxd's vertex emitter had no BREAKC
//  odd-width odd-width-depth
//            a 72x120 RGBA8 target (-depth: with a D24S8 depth buffer) cleared, a red quad drawn
//            into it, then in a pass of its own a green one added over a part of it (-depth:
//            behind the red one's depth); the colour buffer DMAed after each pass (-depth: and
//            the depth buffer after the second): sgxd's passes took widths in 16s only.
//            With depth sgxd asserts: its depth write-back takes no such width
//
// The depth values are upstream Azahar's (Expected); a case without one prints its value.
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "gs_shbin.h"
#include "gsbreak_shbin.h"
#include "vshader_shbin.h"

// All but odd-width-depth: sgxd stops at its first draw by design (a release assert naming the
// case), so it runs only when asked for (CASES=0x1000).
#ifndef CASES
#define CASES 0xfff
#endif

enum { C_MOTION, C_D24S8, C_D16, C_D24, C_D24S8_W, C_D24_W, C_GS_BIG, C_D24S8_HEAP, C_TEX_ONES,
       C_DEPTH_MANY, C_GS_BREAKC, C_ODD_WIDTH, C_ODD_WIDTH_DEPTH, NCASES };
static const char* const names[NCASES] = {"motion", "d24s8", "d16", "d24", "d24s8-w", "d24-w",
                                          "gs-big", "d24s8-heap", "tex-ones", "depth-many",
                                          "gs-breakc", "odd-width", "odd-width-depth"};
// The first pixel's depth (24 bits, D16's 16) as upstream Azahar writes it, and how far ours may
// be from it; ~0u: not known yet, printed.
static const u32 expected[NCASES] = {0, 0xb33332, 0xb332, 0xb33332, 0x2460f5, 0x2460f5, 0, 0xb33332,
                                     0, 0xb33332, 0, 0, 0};
static const u32 tolerance[NCASES] = {0, 0x40, 2, 0x40, 0x4000, 0x4000, 0, 0x40, 0, 0x40, 0, 0, 0};

// OddWidth's sums on upstream Azahar: the colour after each pass, and the depth.
#define ODD_SUMS 0x6c683fe5, 0x822e05fd, 0
#define ODD_DEPTH_SUMS 0x6c683fe5, 0x75c48b65, 0x62baf985

typedef struct {
	float x, y, z, r, g, b, a;
} vertex;

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

static DVLB_s *vs_dvlb, *gs_dvlb, *gsb_dvlb;
static shaderProgram_s vs_prog, gs_prog, gsb_prog;
static int vs_proj, gs_proj, gsb_proj, gsb_limit, gsb_rounds;

static void Attrs(void) {
	C3D_AttrInfo* a = C3D_GetAttrInfo();
	AttrInfo_Init(a);
	AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(a, 1, GPU_FLOAT, 4);
}

static void Buf(void* p) {
	C3D_BufInfo* b = C3D_GetBufInfo();
	BufInfo_Init(b);
	BufInfo_Add(b, p, sizeof(vertex), 2, 0x10);
}

static void Flat(void) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// Two triangles over [x0, x1] x [y0, y1] at z.
static void Quad(vertex* v, float x0, float y0, float x1, float y1, float z, float r, float g,
                 float b) {
	const vertex q[6] = {
		{x0, y0, z, r, g, b, 1}, {x1, y0, z, r, g, b, 1}, {x1, y1, z, r, g, b, 1},
		{x0, y0, z, r, g, b, 1}, {x1, y1, z, r, g, b, 1}, {x0, y1, z, r, g, b, 1},
	};
	memcpy(v, q, sizeof(q));
}

static u32 Motion(char* out) {
	HIDUSER_EnableAccelerometer();
	HIDUSER_EnableGyroscope();
	accelVector a = {0};
	angularRate g = {0};
	for (int i = 0; i < 30; i++) {
		hidScanInput();
		hidAccelRead(&a);
		hidGyroRead(&g);
		gspWaitForVBlank();
	}
	HIDUSER_DisableGyroscope();
	HIDUSER_DisableAccelerometer();
	sprintf(out, "accel %d %d %d, gyro %d %d %d", a.x, a.y, a.z, g.x, g.y, g.z);
	return 0;
}

static u32 Depth(int c, char* out) {
	const bool w = c == C_D24S8_W || c == C_D24_W;
	const GPU_DEPTHBUF format = c == C_D16                    ? GPU_RB_DEPTH16
	                            : c == C_D24 || c == C_D24_W ? GPU_RB_DEPTH24
	                                                         : GPU_RB_DEPTH24_STENCIL8;
	const u32 bpp = format == GPU_RB_DEPTH16 ? 2 : format == GPU_RB_DEPTH24 ? 3 : 4;
	C3D_RenderTarget* t = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, format);
	vertex* v = linearAlloc(9 * sizeof(vertex));
	C3D_Mtx proj;
	u32 n = 6;
	if (w) {
		// A triangle tilted in depth (w 1.5 to 3: a scene's, which sgxd fits its W depths to),
		// then a quad 2 units in front of the eye over it, past the frustum's sides: w 2 over
		// the target.
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), 1.0f, 0.5f, 10.0f, false);
		const vertex tri[3] = {{-1, -1, -1.5f, 1, 0, 0, 1}, {1, -1, -3.0f, 1, 0, 0, 1},
		                       {0, 1, -2.0f, 1, 0, 0, 1}};
		memcpy(v, tri, sizeof(tri));
		Quad(v + 3, -4, -4, 4, 4, -2.0f, 0, 0, 1);
		n = 9;
	} else {
		Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
		Quad(v, 0, 0, 64, 64, 0.3f, 0, 0, 1);
	}
	GSPGPU_FlushDataCache(v, 9 * sizeof(vertex));
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(t);
	C3D_BindProgram(&vs_prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, &proj);
	Attrs();
	Buf(v);
	Flat();
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	// W-buffered: depth = z * scale + w * offset (upstream's), Z: z / w * scale + offset.
	if (w) {
		C3D_DepthMap(false, -0.1f, 0.05f);
	} else {
		C3D_DepthMap(true, -1.0f, 0.0f);
	}
	C3D_DrawArrays(GPU_TRIANGLES, 0, n);
	C3D_FrameEnd(0);
	// The depth buffer by DMA (queued behind the draw): sgxd writes its depth back for the read.
	const u32 bytes = 64 * 64 * bpp;
	u8* dst = linearAlloc(bytes);
	memset(dst, 0xEE, bytes);
	GSPGPU_FlushDataCache(dst, bytes);
	GX_RequestDma((u32*)t->frameBuf.depthBuf, (u32*)dst, bytes);
	gspWaitForDMA();
	GSPGPU_InvalidateDataCache(dst, bytes);
	const u32 mask = bpp == 2 ? 0xFFFFu : 0xFFFFFFu;
	const u32 first = (dst[0] | dst[1] << 8 | (bpp > 2 ? dst[2] << 16 : 0)) & mask;
	u32 differ = 0;
	for (u32 i = 1; i < 64 * 64; i++) {
		const u8* p = dst + i * bpp;
		differ += ((p[0] | p[1] << 8 | (bpp > 2 ? p[2] << 16 : 0)) & mask) != first;
	}
	u32 bad = differ != 0;
	if (expected[c] != ~0u) {
		const u32 d = first > expected[c] ? first - expected[c] : expected[c] - first;
		bad += d > tolerance[c];
		sprintf(out, "depth %06lx (upstream %06lx), %lu pixels differ", (unsigned long)first,
		        (unsigned long)expected[c], (unsigned long)differ);
	} else {
		sprintf(out, "depth %06lx, %lu pixels differ", (unsigned long)first,
		        (unsigned long)differ);
	}
	linearFree(dst);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	return bad;
}

// A depth buffer on the linear heap: the guest's write to its bytes after a pass, then a pass
// again.
static u32 DepthHeap(char* out) {
	const u32 bytes = 64 * 64 * 4;
	C3D_RenderTarget* t = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	void* const vram_depth = t->frameBuf.depthBuf;
	u8* depth = linearAlloc(bytes);
	u8* dst = linearAlloc(bytes);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	memset(depth, 0x11, bytes);
	GSPGPU_FlushDataCache(depth, bytes);
	C3D_FrameBufDepth(&t->frameBuf, depth, GPU_RB_DEPTH24_STENCIL8);
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
	Quad(v, 0, 0, 64, 64, 0.3f, 0, 0, 1);
	GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
	u32 first[3] = {0, 0, 0}, differ[2] = {0, 0};
	for (int round = 0; round < 3; round++) {
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_FrameDrawOn(t);
		C3D_BindProgram(&vs_prog);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, &proj);
		Attrs();
		Buf(v);
		Flat();
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
		C3D_DepthMap(true, -1.0f, 0.0f);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		C3D_FrameEnd(0);
		if (round == 0) {
			// The buffer's memory the guest's again: its bytes, not the pass's depth, are what
			// the DMA copies.
			memset(depth, 0x5A, bytes);
			GSPGPU_FlushDataCache(depth, bytes);
		} else if (round == 2) {
			// Its first bytes alone, on the page the buffer shares with what is before it.
			memset(depth, 0xA5, 8);
			GSPGPU_FlushDataCache(depth, 8);
		}
		memset(dst, 0xEE, bytes);
		GSPGPU_FlushDataCache(dst, bytes);
		GX_RequestDma((u32*)depth, (u32*)dst, bytes);
		gspWaitForDMA();
		GSPGPU_InvalidateDataCache(dst, bytes);
		const u32 mask = round == 1 ? 0xFFFFFFu : 0xFFFFFFFFu;
		first[round] = (dst[0] | dst[1] << 8 | dst[2] << 16 | (u32)dst[3] << 24) & mask;
		for (u32 i = 1; round < 2 && i < 64 * 64; i++) {
			const u8* p = dst + i * 4;
			differ[round] += ((p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) & mask) != first[round];
		}
	}
	const u32 e = expected[C_D24S8_HEAP];
	const u32 d = first[1] > e ? first[1] - e : e - first[1];
	const u32 bad = (first[0] != 0x5A5A5A5Au) + (differ[0] != 0) + (d > tolerance[C_D24S8_HEAP]) +
	                (differ[1] != 0) + (first[2] != 0xA5A5A5A5u);
	sprintf(out,
	        "written %08lx (5a5a5a5a), %lu differ; drawn again %06lx (upstream %06lx), %lu differ; "
	        "first words written %08lx (a5a5a5a5)",
	        (unsigned long)first[0], (unsigned long)differ[0], (unsigned long)first[1],
	        (unsigned long)e, (unsigned long)differ[1], (unsigned long)first[2]);
	C3D_FrameBufDepth(&t->frameBuf, vram_depth, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetDelete(t);
	linearFree(v);
	linearFree(dst);
	linearFree(depth);
	return bad;
}

static u32 GsBig(char* out) {
	enum { Triangles = 6000 };
	C3D_RenderTarget* t = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	vertex* v = linearAlloc(Triangles * 3 * sizeof(vertex));
	// Each pair of triangles the target's two halves, green.
	for (int i = 0; i < Triangles / 2; i++) {
		Quad(v + i * 6, 0, 0, 64, 64, 0.5f, 0, 1, 0);
	}
	GSPGPU_FlushDataCache(v, Triangles * 3 * sizeof(vertex));
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(t);
	C3D_BindProgram(&gs_prog);
	C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, gs_proj, &proj);
	Attrs();
	Buf(v);
	Flat();
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, Triangles * 3);
	C3D_FrameEnd(0);
	// The colour buffer by DMA: a pixel of it green (RGBA8 in memory: A, B, G, R).
	u8* dst = linearAlloc(64 * 64 * 4);
	memset(dst, 0xEE, 64 * 64 * 4);
	GSPGPU_FlushDataCache(dst, 64 * 64 * 4);
	GX_RequestDma((u32*)t->frameBuf.colorBuf, (u32*)dst, 64 * 64 * 4);
	gspWaitForDMA();
	GSPGPU_InvalidateDataCache(dst, 64 * 64 * 4);
	const u32 px = dst[0] | dst[1] << 8 | dst[2] << 16 | (u32)dst[3] << 24;
	sprintf(out, "pixel %08lx", (unsigned long)px);
	const u32 bad = (px & 0xFFFFFF00u) != 0x00FF0000u;
	linearFree(dst);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	return bad;
}

// All-ones in the texture units' parameter registers under a constant-colour combiner.
static u32 TexOnes(char* out) {
	C3D_RenderTarget* t = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	Quad(v, 0, 0, 64, 64, 0.5f, 1, 0, 0);
	GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(t);
	C3D_BindProgram(&vs_prog);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, &proj);
	Attrs();
	Buf(v);
	// Stage 0 the constant, green, in all three sources; the vertices' red reaches nothing.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, 0xFF00FF00);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	GPUCMD_AddWrite(GPUREG_TEXUNIT0_PARAM, 0xFFFFFFFF);
	GPUCMD_AddWrite(GPUREG_TEXUNIT1_PARAM, 0xFFFFFFFF);
	GPUCMD_AddWrite(GPUREG_TEXUNIT2_PARAM, 0xFFFFFFFF);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	// The registers as the other cases have them.
	GPUCMD_AddWrite(GPUREG_TEXUNIT0_PARAM, 0);
	GPUCMD_AddWrite(GPUREG_TEXUNIT1_PARAM, 0);
	GPUCMD_AddWrite(GPUREG_TEXUNIT2_PARAM, 0);
	C3D_FrameEnd(0);
	u8* dst = linearAlloc(64 * 64 * 4);
	memset(dst, 0xEE, 64 * 64 * 4);
	GSPGPU_FlushDataCache(dst, 64 * 64 * 4);
	GX_RequestDma((u32*)t->frameBuf.colorBuf, (u32*)dst, 64 * 64 * 4);
	gspWaitForDMA();
	GSPGPU_InvalidateDataCache(dst, 64 * 64 * 4);
	const u32 px = dst[0] | dst[1] << 8 | dst[2] << 16 | (u32)dst[3] << 24;
	sprintf(out, "pixel %08lx (00ff00ff)", (unsigned long)px);
	const u32 bad = px != 0x00FF00FFu;
	linearFree(dst);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	Flat();
	return bad;
}

// More depth buffers of one size than the GPU side keeps at once.
static u32 DepthMany(char* out) {
	enum { Buffers = 14, W = 256, H = 512, VW = 240, VH = 400 };
	const u32 bytes = W * H * 4;
	C3D_RenderTarget* t = C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	void* const vram_depth = t->frameBuf.depthBuf;
	u8* depth[Buffers];
	for (int i = 0; i < Buffers; i++) {
		depth[i] = linearAlloc(bytes);
		memset(depth[i], 0x11, bytes);
		GSPGPU_FlushDataCache(depth[i], bytes);
	}
	u8* dst = linearAlloc(bytes);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
	Quad(v, 0, 0, 64, 64, 0.3f, 0, 0, 1);
	GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
	for (int i = 0; i < Buffers; i++) {
		C3D_FrameBufDepth(&t->frameBuf, depth[i], GPU_RB_DEPTH24_STENCIL8);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_FrameDrawOn(t);
		// A screen's size inside the power-of-two buffer.
		C3D_SetViewport(0, 0, VW, VH);
		C3D_BindProgram(&vs_prog);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, &proj);
		Attrs();
		Buf(v);
		Flat();
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
		C3D_DepthMap(true, -1.0f, 0.0f);
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
		C3D_FrameEnd(0);
	}
	// Every buffer's depth by DMA: the quad's.
	const u32 e = expected[C_DEPTH_MANY];
	u32 right = 0, first_wrong = 0, wrong_at = Buffers;
	for (int k = 0; k < Buffers; k++) {
		memset(dst, 0xEE, bytes);
		GSPGPU_FlushDataCache(dst, bytes);
		GX_RequestDma((u32*)depth[k], (u32*)dst, bytes);
		gspWaitForDMA();
		GSPGPU_InvalidateDataCache(dst, bytes);
		// A pixel inside the viewport (the viewport's row 0 is the buffer's last).
		const u8* p = dst + (H - 1) * W * 4;
		const u32 z = (p[0] | p[1] << 8 | p[2] << 16) & 0xFFFFFFu;
		const u32 d = z > e ? z - e : e - z;
		if (d <= tolerance[C_DEPTH_MANY]) {
			right++;
		} else if (wrong_at == Buffers) {
			wrong_at = k;
			first_wrong = z;
		}
	}
	const u32 bad = right != Buffers;
	if (bad) {
		sprintf(out, "%lu of %d buffers right; buffer %lu's depth %06lx (upstream %06lx)",
		        (unsigned long)right, Buffers, (unsigned long)wrong_at, (unsigned long)first_wrong,
		        (unsigned long)e);
	} else {
		sprintf(out, "%lu of %d buffers right", (unsigned long)right, Buffers);
	}
	C3D_FrameBufDepth(&t->frameBuf, vram_depth, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetDelete(t);
	linearFree(v);
	linearFree(dst);
	for (int i = 0; i < Buffers; i++) {
		linearFree(depth[i]);
	}
	return bad;
}

// A LOOP left by a BREAKC in a geometry program.
static u32 GsBreakc(char* out) {
	C3D_RenderTarget* t = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	vertex* v = linearAlloc(6 * sizeof(vertex));
	Quad(v, 0, 0, 64, 64, 0.5f, 1, 0, 1);
	GSPGPU_FlushDataCache(v, 6 * sizeof(vertex));
	u8* dst = linearAlloc(64 * 64 * 4);
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, 64.0f, 0.0f, 64.0f, 0.0f, 1.0f, true);
	// The round the loop is left at and the green its rounds make (a quarter each, to 1).
	static const float limits[3] = {2.0f, 3.0f, 100.0f};
	static const u32 greens[3] = {0x80, 0xBF, 0xFF};
	u32 px[3], bad = 0;
	for (int i = 0; i < 3; i++) {
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(t);
		C3D_BindProgram(&gsb_prog);
		C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, gsb_proj, &proj);
		C3D_FVUnifSet(GPU_GEOMETRY_SHADER, gsb_limit, limits[i], 0, 0, 0);
		C3D_IVUnifSet(GPU_GEOMETRY_SHADER, gsb_rounds, 7, 0, 1, 0);
		Attrs();
		Buf(v);
		Flat();
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 6);
		C3D_FrameEnd(0);
		memset(dst, 0xEE, 64 * 64 * 4);
		GSPGPU_FlushDataCache(dst, 64 * 64 * 4);
		GX_RequestDma((u32*)t->frameBuf.colorBuf, (u32*)dst, 64 * 64 * 4);
		gspWaitForDMA();
		GSPGPU_InvalidateDataCache(dst, 64 * 64 * 4);
		// RGBA8 in memory: A, B, G, R.
		px[i] = dst[0] | dst[1] << 8 | dst[2] << 16 | (u32)dst[3] << 24;
		const u32 g = px[i] >> 16 & 0xFF;
		bad += (px[i] & 0xFF00FF00u) != 0 || g + 1 < greens[i] || g > greens[i] + 1;
	}
	sprintf(out, "pixels %08lx %08lx %08lx (green 80, bf, ff)", (unsigned long)px[0],
	        (unsigned long)px[1], (unsigned long)px[2]);
	linearFree(dst);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	return bad != 0;
}

// The bytes' FNV-1a.
static u32 Sum(const u8* p, u32 bytes) {
	u32 h = 0x811C9DC5u;
	for (u32 i = 0; i < bytes; i++) {
		h = (h ^ p[i]) * 0x01000193u;
	}
	return h;
}

// A colour buffer 72 wide: its passes, from a clear and from its own bytes. The sums are
// upstream Azahar's.
static u32 OddWidth(bool with_depth, char* out) {
	enum { W = 72, H = 120, Bytes = W * H * 4 };
	C3D_RenderTarget* t =
		C3D_RenderTargetCreate(W, H, GPU_RB_RGBA8, with_depth ? GPU_RB_DEPTH24_STENCIL8 : -1);
	vertex* v = linearAlloc(12 * sizeof(vertex));
	// The picture is 120x72: red over 8..100 x 8..60 nearer, green over 50..110 x 30..70.
	Quad(v, 8, 8, 100, 60, 0.3f, 1, 0, 0);
	Quad(v + 6, 50, 30, 110, 70, 0.6f, 0, 1, 0);
	GSPGPU_FlushDataCache(v, 12 * sizeof(vertex));
	u8* dst = linearAlloc(Bytes);
	C3D_Mtx proj;
	Mtx_OrthoTilt(&proj, 0.0f, (float)H, 0.0f, (float)W, 0.0f, 1.0f, true);
	u32 sums[3] = {0, 0, 0}, counts[2][5];
	for (int pass = 0; pass < 2; pass++) {
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (pass == 0) {
			C3D_RenderTargetClear(t, C3D_CLEAR_ALL, 0x000000FF, 0);
		}
		C3D_FrameDrawOn(t);
		C3D_BindProgram(&vs_prog);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, vs_proj, &proj);
		Attrs();
		Buf(v);
		Flat();
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DepthMap(true, -1.0f, 0.0f);
		if (with_depth) {
			// The red quad's depth 0.7 over the clear's 0; the green one's 0.4 passes where it
			// is the greater: outside the red one.
			C3D_DepthTest(true, pass == 0 ? GPU_ALWAYS : GPU_GREATER, GPU_WRITE_ALL);
		} else {
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			if (pass == 1) {
				C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
			}
		}
		C3D_DrawArrays(GPU_TRIANGLES, pass * 6, 6);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		C3D_FrameEnd(0);
		memset(dst, 0xEE, Bytes);
		GSPGPU_FlushDataCache(dst, Bytes);
		GX_RequestDma((u32*)t->frameBuf.colorBuf, (u32*)dst, Bytes);
		gspWaitForDMA();
		GSPGPU_InvalidateDataCache(dst, Bytes);
		sums[pass] = Sum(dst, Bytes);
		// Black, red, green, yellow, anything else.
		memset(counts[pass], 0, sizeof(counts[pass]));
		for (u32 i = 0; i < W * H; i++) {
			const u32 px = dst[4 * i] | dst[4 * i + 1] << 8 | dst[4 * i + 2] << 16 |
			               (u32)dst[4 * i + 3] << 24;
			counts[pass][px == 0x000000FFu   ? 0
			             : px == 0xFF0000FFu ? 1
			             : px == 0x00FF00FFu ? 2
			             : px == 0xFFFF00FFu ? 3
			                                 : 4]++;
		}
	}
	if (with_depth) {
		memset(dst, 0xEE, Bytes);
		GSPGPU_FlushDataCache(dst, Bytes);
		GX_RequestDma((u32*)t->frameBuf.depthBuf, (u32*)dst, Bytes);
		gspWaitForDMA();
		GSPGPU_InvalidateDataCache(dst, Bytes);
		// The depths' top 8 bits (upstream's and the SGX's 24 differ in the last ones).
		for (u32 i = 0; i < W * H; i++) {
			dst[4 * i] = 0;
			dst[4 * i + 1] = 0;
			dst[4 * i + 3] = 0;
		}
		sums[2] = Sum(dst, Bytes);
	}
	// Upstream's: the two passes' colour (and the depth).
	static const u32 want[2][3] = {{ODD_SUMS}, {ODD_DEPTH_SUMS}};
	const u32* w = want[with_depth];
	const u32* c = counts[1];
	sprintf(out, "sums %08lx %08lx %08lx (%08lx %08lx %08lx); black %lu red %lu green %lu yellow "
	             "%lu other %lu",
	        (unsigned long)sums[0], (unsigned long)sums[1], (unsigned long)sums[2],
	        (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)c[0],
	        (unsigned long)c[1], (unsigned long)c[2], (unsigned long)c[3], (unsigned long)c[4]);
	linearFree(dst);
	linearFree(v);
	C3D_RenderTargetDelete(t);
	return sums[0] != w[0] || sums[1] != w[1] || sums[2] != w[2];
}

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	vs_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&vs_prog);
	shaderProgramSetVsh(&vs_prog, &vs_dvlb->DVLE[0]);
	vs_proj = shaderInstanceGetUniformLocation(vs_prog.vertexShader, "projection");
	gs_dvlb = DVLB_ParseFile((u32*)gs_shbin, gs_shbin_size);
	shaderProgramInit(&gs_prog);
	shaderProgramSetVsh(&gs_prog, &gs_dvlb->DVLE[0]);
	shaderProgramSetGsh(&gs_prog, &gs_dvlb->DVLE[1], 6);
	gs_proj = shaderInstanceGetUniformLocation(gs_prog.geometryShader, "projection");
	gsb_dvlb = DVLB_ParseFile((u32*)gsbreak_shbin, gsbreak_shbin_size);
	shaderProgramInit(&gsb_prog);
	shaderProgramSetVsh(&gsb_prog, &gsb_dvlb->DVLE[0]);
	shaderProgramSetGsh(&gsb_prog, &gsb_dvlb->DVLE[1], 6);
	gsb_proj = shaderInstanceGetUniformLocation(gsb_prog.geometryShader, "projection");
	gsb_limit = shaderInstanceGetUniformLocation(gsb_prog.geometryShader, "limit");
	gsb_rounds = shaderInstanceGetUniformLocation(gsb_prog.geometryShader, "rounds");

	char line[300], detail[220];
	u32 failed = 0, ran = 0;
	printf("\x1b[1;1Hcrash_cases_test (cases %#x)\n", CASES);
	for (int c = 0; c < NCASES; c++) {
		if (!(CASES & (1u << c))) {
			continue;
		}
		snprintf(line, sizeof(line), "crash_cases_test: %s starts", names[c]);
		Log(line);
		detail[0] = 0;
		const u32 bad = c == C_MOTION         ? Motion(detail)
		                : c == C_GS_BIG       ? GsBig(detail)
		                : c == C_D24S8_HEAP   ? DepthHeap(detail)
		                : c == C_TEX_ONES     ? TexOnes(detail)
		                : c == C_DEPTH_MANY   ? DepthMany(detail)
		                : c == C_GS_BREAKC    ? GsBreakc(detail)
		                : c == C_ODD_WIDTH    ? OddWidth(false, detail)
		                : c == C_ODD_WIDTH_DEPTH ? OddWidth(true, detail)
		                                      : Depth(c, detail);
		failed += bad;
		ran++;
		snprintf(line, sizeof(line), "crash_cases_test: %-8s %s: %s", names[c], bad ? "FAILED" : "ok",
		         detail);
		Log(line);
		printf("%s\n", line + 18);
	}
	snprintf(line, sizeof(line), "crash_cases_test: %s %lu (%lu cases)", failed ? "FAILED" : "ALL OK",
	         (unsigned long)failed, (unsigned long)ran);
	Log(line);
	printf("\n%s\n", line);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	shaderProgramFree(&gsb_prog);
	shaderProgramFree(&gs_prog);
	shaderProgramFree(&vs_prog);
	DVLB_Free(gsb_dvlb);
	DVLB_Free(gs_dvlb);
	DVLB_Free(vs_dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
