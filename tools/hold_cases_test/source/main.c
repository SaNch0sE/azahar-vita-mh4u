// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// hold_cases_test: the guest's loads and stores beside the bytes of queued GPU work, in each case
// where azahar's page holds judged them by their 4 KiB page instead of the bytes the work uses
// (the access then waited for the GPU, or had a picture copied back and the GPU drained). Each
// case runs in blocks of BLOCK frames, in turn, its GPU work queued (BUSY full-screen quads into
// a 240x400 target in VRAM) while the CPU makes the accesses. The values loaded and the bytes
// left are checked against the same accesses on plain memory, and the pixels where they are read.
//
// Whether an access waited is the emulator's to tell (diagnostics, sgxd_flags `perflog holdlog`:
// the aborts that waited, by page and PC): the guest's clock stops while the emulator handles an
// abort, so svcGetSystemTick does not see a wait. Each case has its own page:
//
//  budget    G: 128 stores and loads beside a display transfer's output (no screen's, from
//            CPU-written pixels, no picture), more than 32 a hold
//  display   P: stores beside the top screen's transfer output (from CPU-written pixels, no
//            picture) on a page a draw's vertices are read from too
//  hle       P: an HLE write (CFG's GetConfigInfoBlk2 into our buffer) beside that output
//  watch     R: stores beside a render target's rows after a read copied them back
//  version   R2: stores beside a render target's rows on a versioned page a transfer reads
//            (the page versioned at each block's start; run with limit=100, or the vblanks of
//            an uncapped run send the version home between frames)
//  vram-rt   V (VRAM): stores beside a render target
//  vram-tex  W (VRAM): stores beside a texture a draw reads
//  form      G: one ARM or Thumb form a frame (forms_arm.s, forms_thumb.s): an RRX offset, LDR
//            and LDM to the PC, LDR to the PC post-indexed, STM with its base in the list, STR
//            and STM of the PC, LDREX/STREX (word, byte, half, double), an LDREX alone; Thumb
//            STMIA with its base in the list, POP with the PC
//  cross     one form a frame across the boundaries g0|G (both written by transfers) and u|g0
//            (u unheld): STM, LDM, STRD, LDRD, VSTM, VLDM, Thumb STMIA, LDMIA, PUSH, POP
//  x-held    X: one form a frame from the page before X (a transfer's output, done: in CDRAM)
//            into the first bytes of X, a 32x32 RGB565 transfer output queued behind the busy
//            draws: a six-word STMIA (five words before) and a VSTMIA of s0-s5 (three before),
//            which wait for the transfer. The A9 reports X's address unless the page before
//            faults too (that page faulting with no hold left on it is not made here)
//  idx8      I: 128 stores and loads beside an 8-bit index list a queued draw reads (azahar's
//            render thread widens it on the CPU; the GPU never reads it)
//  idx8-onto J: the 8-bit list rewritten (one vertex six times: nothing drawn) right after its
//            draw is queued: the draw takes the list it was queued with, its 64x64 target red
//  idx8-ver  K: as idx8-onto, the list beside its draw's vertices, which the GPU reads in place:
//            the page moves to CDRAM and its writes take versions (azahar's render thread reads
//            the list through the guest's view, which a version moves)
//
// The bottom screen shows each case's and form's frames and failed checks; the lines go to the
// debug output too. After RUN frames the last line says ALL OK or how many checks failed and
// stays (Start quits).
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define BUSY 200
#define BLOCK 6
#define RUN 900

#define DIM(w, h) GX_BUFFER_DIM((w), (h))
#define TRANSFER_RGB8                                                                      \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |   \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define TRANSFER_RGBA8                                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |   \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TRANSFER_RGB565                                                                    \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |   \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// A pixel as Word reads an RGBA8 buffer (A, B, G, R in memory).
#define Red 0xFF0000FFu

typedef void Form(u8* base, u32* out);
Form f_rrx, f_ldr_pc, f_ldr_pc_thumb, f_ldr_pc_post, f_ldm_pc, f_stm_base, f_str_pc, f_stm_pc,
	f_ldrex, f_ldrexb, f_ldrexh, f_ldrexd, f_ldrex_alone, t_stm_base, t_pop_pc;
Form x_stm, x_ldm, x_strd, x_ldrd, x_vstm, x_vldm, xt_stmia, xt_ldmia, xt_push, xt_pop;
Form xh_stm6, xh_vstm6;

typedef struct {
	const char* name;
	Form* run;
	u32 ref[4];
	u32 frames, fails;
} FormCase;

static FormCase forms[] = {
	{"rrx", f_rrx},         {"ldr-pc", f_ldr_pc},     {"ldr-pc-t", f_ldr_pc_thumb},
	{"ldr-pc-post", f_ldr_pc_post}, {"ldm-pc", f_ldm_pc}, {"stm-base", f_stm_base},
	{"str-pc", f_str_pc},   {"stm-pc", f_stm_pc},     {"ldrex", f_ldrex},
	{"ldrexb", f_ldrexb},   {"ldrexh", f_ldrexh},     {"ldrexd", f_ldrexd},
	{"ldrex-alone", f_ldrex_alone}, {"t-stm-base", t_stm_base}, {"t-pop-pc", t_pop_pc},
};
static FormCase crosses[] = {
	{"x-stm", x_stm},   {"x-ldm", x_ldm},     {"x-strd", x_strd},   {"x-ldrd", x_ldrd},
	{"x-vstm", x_vstm}, {"x-vldm", x_vldm},   {"xt-stmia", xt_stmia}, {"xt-ldmia", xt_ldmia},
	{"xt-push", xt_push}, {"xt-pop", xt_pop},
};
static FormCase xhelds[] = {
	{"xh-stm6", xh_stm6}, {"xh-vstm6", xh_vstm6},
};
#define FORMS (sizeof(forms) / sizeof(forms[0]))
#define CROSSES (sizeof(crosses) / sizeof(crosses[0]))
#define XHELDS (sizeof(xhelds) / sizeof(xhelds[0]))
#define ALL_FORMS (FORMS + CROSSES + XHELDS)

// Form i of forms, crosses and xhelds in turn.
static FormCase* FormAt(u32 i) {
	return i < FORMS ? &forms[i] : i < FORMS + CROSSES ? &crosses[i - FORMS]
	                                                   : &xhelds[i - FORMS - CROSSES];
}

enum { C_BUDGET, C_DISPLAY, C_HLE, C_WATCH, C_VERSION, C_VRAM_RT, C_VRAM_TEX, C_FORM, C_CROSS,
       C_IDX8, C_IDX8_ONTO, C_IDX8_VER, C_XHELD, CASES };
static const char* const case_names[CASES] = {"budget", "display", "hle", "watch", "version",
                                              "vram-rt", "vram-tex", "form", "cross", "idx8",
                                              "idx8-onto", "idx8-ver", "x-held"};
static u32 frames_of[CASES], fails_of[CASES];

typedef struct { float x, y, z, r, g, b, a, u, v; } vertex;

static void Quad(vertex* v, float r, float g, float b) {
	const vertex q[6] = {
		{0, 0, 0.5f, r, g, b, 1, 0, 0},     {400, 0, 0.5f, r, g, b, 1, 1, 0},
		{400, 240, 0.5f, r, g, b, 1, 1, 1}, {0, 0, 0.5f, r, g, b, 1, 0, 0},
		{400, 240, 0.5f, r, g, b, 1, 1, 1}, {0, 240, 0.5f, r, g, b, 1, 0, 1},
	};
	memcpy(v, q, sizeof(q));
}

static u32 Word(const volatile u8* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void Log(const char* text) {
	svcOutputDebugString(text, strlen(text));
}

// The pages, each case's bytes at +0x400 (or the boundaries' 16), the GPU's elsewhere:
static u8* vb;   // a black quad at +0, the busy draws' vertices at +0x800, one draw's at +0x1800
static u8* gx;   // u (unheld) | g0: a 32x32 RGB565 transfer output at +0x400 | G: a 64x64 RGBA8 one at +0x800
static u8* src;  // the transfers' input for g0 and G (64x64 RGBA8, written by the CPU)
static u8* pp;   // P: one draw's vertices at +0x100, the top screen (240x400 RGB8) at +0x800
static u8* top;  // the top screen's transfer input (240x400 RGBA8, written by the CPU)
static u8* rr;   // R: a 64x64 RGBA8 render target at +0x800 (watch)
static u8* r2;   // R2: a 64x64 RGBA8 render target at +0x800 (version)
static u8* zz;   // a 64x64 RGBA8 transfer output from R2's target
static u8* vv;   // VRAM: a 64x64 RGBA8 render target at +0x800
static u8* ww;   // VRAM: a 64x64 RGBA8 texture at +0x800
static u8* ib;   // I: an 8-bit index list at +0 (idx8)
static u8* ic;   // J: an 8-bit index list at +0 (idx8-onto)
static u8* ik;   // K: an 8-bit index list at +0, its draw's red quad at +0x100 (idx8-ver)
static u8* r8;   // a 64x64 RGBA8 render target at +0x800 J's draw fills
static u8* xh;   // a 32x32 RGB565 transfer output at +0x800 | X: another at +0 (x-held)
static u8 plain[0x4000] __attribute__((aligned(0x1000)));

#define G (gx + 0x2000)
#define G0 (gx + 0x1000)

static C3D_RenderTarget *busy, *t_watch, *t_version, *t_vram, *t_idx;
static C3D_Tex tex_vram;
static int uLoc_projection;
static C3D_Mtx projection;

static void Buf(void* p) {
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, p, sizeof(vertex), 3, 0x210);
}

static void Untextured(void) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static void Textured(void) {
	C3D_TexBind(0, &tex_vram);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static void On(C3D_RenderTarget* t) {
	C3D_FrameDrawOn(t);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
}

// One quad of the vertices at v into t.
static void Draw(C3D_RenderTarget* t, void* v) {
	On(t);
	Untextured();
	Buf(v);
	C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

// BUSY grey quads into the busy target: the GPU's work the accesses run beside.
static void Busy(void) {
	On(busy);
	Untextured();
	Buf(vb + 0x800);
	for (int i = 0; i < BUSY; i++) {
		C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
	}
}

// After the frame's draws: the transfers into g0 and G (queued behind the draws).
static void TransfersToG(void) {
	C3D_FrameSplit(0);
	GX_DisplayTransfer((u32*)src, DIM(32, 32), (u32*)(G0 + 0x400), DIM(32, 32), TRANSFER_RGB565);
	GX_DisplayTransfer((u32*)src, DIM(64, 64), (u32*)(G + 0x800), DIM(64, 64), TRANSFER_RGBA8);
}

// n stores, then n loads, of words [p, p + 4n): the values of round k.
static u32 Words(volatile u32* p, int n, u32 k) {
	u32 bad = 0;
	for (int i = 0; i < n; i++) {
		p[i] = 0x5A000000u ^ (k << 12) ^ (u32)i;
	}
	for (int i = 0; i < n; i++) {
		bad += p[i] != (0x5A000000u ^ (k << 12) ^ (u32)i);
	}
	return bad;
}

static u32 RunForm(FormCase* f, u8* base) {
	u32 out[4] = {0};
	f->run(base, out);
	const u32 bad = memcmp(out, f->ref, sizeof(out)) != 0;
	f->frames++;
	f->fails += bad;
	return bad;
}

static void Report(u32 frame) {
	char line[160];
	int row = 3;
	for (int c = 0; c < CASES; c++, row++) {
		snprintf(line, sizeof(line), "hold_cases_test: frame %lu: %-8s %lu frames, %lu failed",
		         (unsigned long)frame, case_names[c], (unsigned long)frames_of[c],
		         (unsigned long)fails_of[c]);
		Log(line);
		printf("\x1b[%d;1H%-9s %4lu frames %lu failed\x1b[K", row, case_names[c],
		       (unsigned long)frames_of[c], (unsigned long)fails_of[c]);
	}
	for (u32 i = 0; i < ALL_FORMS; i++) {
		const FormCase* f = FormAt(i);
		snprintf(line, sizeof(line), "hold_cases_test: frame %lu: %-12s %lu runs, %lu failed",
		         (unsigned long)frame, f->name, (unsigned long)f->frames, (unsigned long)f->fails);
		Log(line);
	}
	// The forms two to a row: failed/runs.
	for (u32 i = 0; i < ALL_FORMS; i += 2, row++) {
		const FormCase* a = FormAt(i);
		const FormCase* b = i + 1 < ALL_FORMS ? FormAt(i + 1) : NULL;
		printf("\x1b[%d;1H%-11s %lu/%lu  ", row, a->name, (unsigned long)a->fails,
		       (unsigned long)a->frames);
		if (b) {
			printf("%-11s %lu/%lu", b->name, (unsigned long)b->fails, (unsigned long)b->frames);
		}
		printf("\x1b[K");
	}
}

// The version case's page: the guest's write into the target's rows while a transfer reads
// them, once the rows are the guest's (a read copied them back, a write marked them written),
// versions the page; each frame of the block then draws the target (its picture owns the rows
// again) and, once that is done, transfers from it: the page read, no write queued. Called in a
// frame, which it ends; it leaves one begun.
static void VersionSetup(void) {
	Draw(t_version, vb + 0x1800);
	C3D_FrameEnd(0);
	C3D_FrameBegin(0);
	volatile u32* const row = (volatile u32*)(r2 + 0x800);
	const u32 v = *row;
	*row = v;
	Busy();
	C3D_FrameSplit(0);
	GX_DisplayTransfer((u32*)(r2 + 0x800), DIM(64, 64), (u32*)zz, DIM(64, 64), TRANSFER_RGBA8);
	C3D_FrameEnd(0);
	*row = v;
	C3D_FrameBegin(0);
}

int main() {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);
	cfguInit();

	vb = linearMemAlign(0x2000, 0x1000);
	gx = linearMemAlign(0x7000, 0x1000);
	src = linearMemAlign(0x4000, 0x1000);
	pp = linearMemAlign(0x47000, 0x1000);
	top = linearMemAlign(240 * 400 * 4, 0x1000);
	rr = linearMemAlign(0x5000, 0x1000);
	r2 = linearMemAlign(0x5000, 0x1000);
	zz = linearMemAlign(0x4000, 0x1000);
	vv = vramMemAlign(0x5000, 0x1000);
	ww = vramMemAlign(0x5000, 0x1000);
	ib = linearMemAlign(0x1000, 0x1000);
	ic = linearMemAlign(0x1000, 0x1000);
	ik = linearMemAlign(0x1000, 0x1000);
	r8 = linearMemAlign(0x5000, 0x1000);
	xh = linearMemAlign(0x2000, 0x1000);
	if (!vb || !gx || !src || !pp || !top || !rr || !r2 || !zz || !vv || !ww || !ib || !ic ||
	    !ik || !r8 || !xh) {
		Log("hold_cases_test: an allocation failed");
		printf("allocation failed\n");
		while (aptMainLoop()) {
			gspWaitForVBlank();
		}
		return 1;
	}
	memset(vb, 0, 0x2000);
	memset(gx, 0, 0x7000);
	memset(pp, 0, 0x800);
	memset(rr, 0, 0x800);
	memset(r2, 0, 0x800);
	memset(vv, 0, 0x800);
	memset(ww, 0, 0x800);
	memset(plain, 0, sizeof(plain));
	memset(ib, 0, 0x1000);
	memset(ic, 0, 0x1000);
	memset(ik, 0, 0x1000);
	Quad((vertex*)(ik + 0x100), 1, 0, 0);
	memset(r8, 0, 0x800);
	for (u32 i = 0; i < 6; i++) {
		ib[i] = (u8)i;
		ic[i] = (u8)i;
		ik[i] = (u8)i;
	}
	GSPGPU_FlushDataCache(ik, 0x1000);
	GSPGPU_FlushDataCache(ib, 0x1000);
	GSPGPU_FlushDataCache(ic, 0x1000);
	GSPGPU_FlushDataCache(r8, 0x5000);
	Quad((vertex*)vb, 0, 0, 0);
	Quad((vertex*)(vb + 0x800), 0.25f, 0.25f, 0.25f);
	Quad((vertex*)(vb + 0x1800), 1, 0, 0);
	Quad((vertex*)(pp + 0x100), 0.5f, 0.5f, 0.5f);
	memset(xh, 0, 0x2000);
	GSPGPU_FlushDataCache(xh, 0x2000);
	for (u32 i = 0; i < 64 * 64; i++) {
		((u32*)src)[i] = 0x10203040u + i;
		((u32*)(ww + 0x800))[i] = 0x00FF00FFu;
	}
	for (u32 i = 0; i < 240 * 400; i++) {
		((u32*)top)[i] = 0xFF000000u | (i * 2654435761u >> 8);
	}
	GSPGPU_FlushDataCache(vb, 0x2000);
	GSPGPU_FlushDataCache(gx, 0x7000);
	GSPGPU_FlushDataCache(src, 0x4000);
	GSPGPU_FlushDataCache(pp, 0x47000);
	GSPGPU_FlushDataCache(top, 240 * 400 * 4);
	GSPGPU_FlushDataCache(rr, 0x5000);
	GSPGPU_FlushDataCache(r2, 0x5000);

	busy = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
	t_watch = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	t_watch->frameBuf.colorBuf = rr + 0x800;
	t_version = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	t_version->frameBuf.colorBuf = r2 + 0x800;
	t_vram = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	t_vram->frameBuf.colorBuf = vv + 0x800;
	t_idx = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, -1);
	t_idx->frameBuf.colorBuf = r8 + 0x800;
	C3D_TexInitVRAM(&tex_vram, 64, 64, GPU_RGBA8);
	tex_vram.data = ww + 0x800;

	DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgram_s program;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);
	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4);
	AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 2);
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	// The references on plain memory, never the GPU's.
	for (u32 i = 0; i < FORMS; i++) {
		forms[i].run(plain + 0x400, forms[i].ref);
	}
	for (u32 i = 0; i < CROSSES; i++) {
		crosses[i].run(plain + 0x2000, crosses[i].ref);
	}
	for (u32 i = 0; i < XHELDS; i++) {
		xhelds[i].run(plain + 0x2000, xhelds[i].ref);
	}
	u8 ref_hle[4] __attribute__((aligned(4)));
	CFGU_GetConfigInfoBlk2(4, 0x000B0000, ref_hle);

	// The top screen shows P's buffer: a transfer into it is the display's.
	gfxSwapBuffers();
	C3D_FrameBegin(0);
	GX_DisplayTransfer((u32*)top, DIM(240, 400), (u32*)(pp + 0x800), DIM(240, 400), TRANSFER_RGB8);
	C3D_FrameEnd(0);
	C3D_FrameBegin(0);
	C3D_FrameEnd(0);
	gspPresentBuffer(GFX_TOP, 0, pp + 0x800, pp + 0x800, 240 * 3, GSP_BGR8_OES);
	gspWaitForVBlank();
	gspWaitForVBlank();

	char line[220];
	snprintf(line, sizeof(line), "hold_cases_test: vb %08lx, u %08lx, g0 %08lx, G %08lx, P %08lx, "
	         "R %08lx, R2 %08lx, V %08lx, W %08lx, I %08lx, J %08lx; %d busy quads a frame, %d "
	         "frames a block",
	         (unsigned long)osConvertVirtToPhys(vb), (unsigned long)osConvertVirtToPhys(gx),
	         (unsigned long)osConvertVirtToPhys(G0), (unsigned long)osConvertVirtToPhys(G),
	         (unsigned long)osConvertVirtToPhys(pp), (unsigned long)osConvertVirtToPhys(rr),
	         (unsigned long)osConvertVirtToPhys(r2), (unsigned long)osConvertVirtToPhys(vv),
	         (unsigned long)osConvertVirtToPhys(ww), (unsigned long)osConvertVirtToPhys(ib),
	         (unsigned long)osConvertVirtToPhys(ic), BUSY, BLOCK);
	Log(line);
	snprintf(line, sizeof(line), "hold_cases_test: K %08lx, X %08lx",
	         (unsigned long)osConvertVirtToPhys(ik), (unsigned long)osConvertVirtToPhys(xh + 0x1000));
	Log(line);
	printf("\x1b[1;1Hhold_cases_test\x1b[K");

	u32 frame = 0, form_next = 0, cross_next = 0, xheld_next = 0;
	while (aptMainLoop() && frame < RUN) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		const int c = (frame / BLOCK) % CASES;
		const bool first = frame % BLOCK == 0;
		const u32 k = frame;
		frame++;
		u32 bad = 0;
		C3D_FrameBegin(0);
		switch (c) {
		case C_BUDGET:
			Busy();
			TransfersToG();
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(G + 0x400), 64, k) +
			      Words((volatile u32*)(G + 0x400), 64, k + 1);
			break;
		case C_DISPLAY:
		case C_HLE:
			Draw(busy, pp + 0x100);
			Busy();
			C3D_FrameSplit(0);
			GX_DisplayTransfer((u32*)top, DIM(240, 400), (u32*)(pp + 0x800), DIM(240, 400),
			                   TRANSFER_RGB8);
			C3D_FrameEnd(0);
			if (c == C_DISPLAY) {
				bad = Words((volatile u32*)(pp + 0x400), 16, k);
			} else {
				u8 got[4] __attribute__((aligned(4)));
				CFGU_GetConfigInfoBlk2(4, 0x000B0000, pp + 0x480);
				memcpy(got, pp + 0x480, 4);
				bad = memcmp(got, ref_hle, 4) != 0;
			}
			break;
		case C_WATCH: {
			Draw(t_watch, vb + 0x800);
			Busy();
			C3D_FrameEnd(0);
			// A read of the target's rows: copied back, the page watched for the first write.
			const u32 px = Word(rr + 0x800);
			C3D_FrameBegin(0);
			Busy();
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(rr + 0x400), 16, k);
			// The grey quad's colour, read from the target.
			bad += (px & 0xFFFFFF00u) != 0x40404000u && (px & 0xFFFFFF00u) != 0x3F3F3F00u;
			break;
		}
		case C_VERSION:
			if (first) {
				VersionSetup();
			}
			Draw(t_version, vb + 0x1800);
			C3D_FrameEnd(0);
			C3D_FrameBegin(0);
			Busy();
			C3D_FrameSplit(0);
			GX_DisplayTransfer((u32*)(r2 + 0x800), DIM(64, 64), (u32*)zz, DIM(64, 64),
			                   TRANSFER_RGBA8);
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(r2 + 0x400), 16, k);
			break;
		case C_VRAM_RT:
			Draw(t_vram, vb + 0x1800);
			Busy();
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(vv + 0x400), 16, k);
			break;
		case C_VRAM_TEX:
			On(busy);
			Textured();
			Buf(vb + 0x1800);
			C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
			Busy();
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(ww + 0x400), 16, k);
			break;
		case C_FORM:
			Busy();
			TransfersToG();
			C3D_FrameEnd(0);
			bad = RunForm(&forms[form_next++ % FORMS], G + 0x400);
			break;
		case C_CROSS: {
			Busy();
			TransfersToG();
			C3D_FrameEnd(0);
			// Both boundaries a frame, the form counted once.
			FormCase* const f = &crosses[cross_next++ % CROSSES];
			bad = RunForm(f, G) + RunForm(f, G0);
			f->frames--;
			break;
		}
		case C_IDX8:
			On(busy);
			Untextured();
			Buf(vb + 0x1800);
			C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_BYTE, ib);
			Busy();
			C3D_FrameEnd(0);
			bad = Words((volatile u32*)(ib + 0x400), 64, k) +
			      Words((volatile u32*)(ib + 0x400), 64, k + 1);
			break;
		case C_IDX8_ONTO:
		case C_IDX8_VER: {
			u8* const at = c == C_IDX8_ONTO ? ic : ik;
			// Black first (a draw: azahar fills only VRAM on the GPU), then the list's red quad.
			Draw(t_idx, vb);
			Buf(c == C_IDX8_ONTO ? vb + 0x1800 : ik + 0x100);
			C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_BYTE, at);
			Busy();
			C3D_FrameEnd(0);
			// Rewritten while its draw is queued: one vertex six times, nothing drawn.
			volatile u8* const list = at;
			for (u32 i = 0; i < 6; i++) {
				list[i] = 0;
			}
			// The draw's pixel (the read waits for the draw, its target's page written).
			const u32 px = Word(r8 + 0x800);
			bad = (px & 0xFFFFFF00u) != (Red & 0xFFFFFF00u);
			for (u32 i = 0; i < 6; i++) {
				list[i] = (u8)i;
			}
			GSPGPU_FlushDataCache(at, 0x10);
			break;
		}
		case C_XHELD:
			// The page before X a transfer's output, done: moved to CDRAM (the GPU overwrote it),
			// uncached and unheld, as the reports' first pages were (0x2xxxxxxx, TEX 100 CB 00).
			GX_DisplayTransfer((u32*)src, DIM(32, 32), (u32*)(xh + 0x800), DIM(32, 32),
			                   TRANSFER_RGB565);
			C3D_FrameEnd(0);
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			// The store reaches bytes the queued transfer writes: it waits on X's holds.
			Busy();
			C3D_FrameSplit(0);
			GX_DisplayTransfer((u32*)src, DIM(32, 32), (u32*)(xh + 0x1000), DIM(32, 32),
			                   TRANSFER_RGB565);
			C3D_FrameEnd(0);
			bad = RunForm(&xhelds[xheld_next++ % XHELDS], xh + 0x1000);
			break;
		}
		frames_of[c]++;
		fails_of[c] += bad;
		if (frame % (CASES * BLOCK) == 0) {
			Report(frame);
		}
	}
	// The VRAM target's pixel: red.
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_FrameEnd(0);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_FrameEnd(0);
	const u32 vram_px = Word(vv + 0x800);
	const u32 pixel_fails = (vram_px & 0xFFFFFF00u) != (Red & 0xFFFFFF00u);
	Report(frame);
	u32 failed = pixel_fails;
	for (int c = 0; c < CASES; c++) {
		failed += fails_of[c];
	}
	snprintf(line, sizeof(line), "hold_cases_test: %s %lu (%lu frames; VRAM target %08lx)",
	         failed ? "FAILED" : "ALL OK", (unsigned long)failed, (unsigned long)frame,
	         (unsigned long)vram_px);
	Log(line);
	printf("\x1b[30;1H%s\x1b[K", line);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) {
			break;
		}
		gspWaitForVBlank();
	}
	cfguExit();
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C3D_Fini();
	gfxExit();
	return 0;
}
