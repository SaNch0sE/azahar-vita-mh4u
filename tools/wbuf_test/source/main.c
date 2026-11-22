// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// wbuf_test: Z-buffered draws testing depth against W-buffered ones. The PICA takes
// the depth mode per draw, and both modes write plain numbers into the one depth buffer: a W-buffered draw (z * scale + w * offset)
// and a Z-buffered one (z / w * scale + offset) compare as those numbers.
//
// The top screen: a W-buffered ground through a perspective projection (near 1, far 100, the
// PICA's clip space: z from 0 at the near plane to -w at the far one), depth scale -0.01, so its
// depth is 0.0101 * (w - 1); then four Z-buffered bands across it at the PICA depths 0.1, 0.3,
// 0.5 and 0.8 (depth test less, no depth write). Each band covers the ground past the w where the
// ground's depth reaches its own, 10.9, 30.7, 50.5 and 80.2, and the sky (the depth clear, 1).
// The ground is the eye-space plane y = -10, so the w at a row is -10 / its clip y: the band's
// edge is at y = -10 / w_edge, -0.92, -0.33, -0.20 and -0.12 of the half-height below the
// horizon. The bottom screen lists them. Start quits.

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x405080ff
#define DISPLAY_TRANSFER_FLAGS                                                                    \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define NEAR 1.0f
#define FAR 100.0f
#define GROUND_Y -10.0f
#define GRID_X 24
#define GRID_Z 64
#define W_FIRST 8.0f
#define W_LAST 95.0f
#define BANDS 4
#define VERTS (GRID_X * GRID_Z * 6 + BANDS * 6)

typedef struct {
    float pos[4];
    float clr[4];
} Vertex;

static const float band_depth[BANDS] = {0.1f, 0.3f, 0.5f, 0.8f};
static const float band_colour[BANDS][4] = {
    {0.9f, 0.1f, 0.1f, 1.0f}, {0.9f, 0.9f, 0.1f, 1.0f}, {0.1f, 0.9f, 0.9f, 1.0f},
    {0.9f, 0.1f, 0.9f, 1.0f}};

// The PICA's clip space of an eye-space point on the ground: x and y as is (a 90-degree view),
// w = -z_eye, and z = a * w + b, 0 at the near plane and -w at the far one.
static void Ground(Vertex* v, float x, float w, const float* colour) {
    const float a = -FAR / (FAR - NEAR), b = FAR * NEAR / (FAR - NEAR);
    v->pos[0] = x;
    v->pos[1] = GROUND_Y;
    v->pos[2] = a * w + b;
    v->pos[3] = w;
    for (int c = 0; c < 4; c++) {
        v->clr[c] = colour[c];
    }
}

static void Fill(Vertex* v) {
    static const float light[4] = {0.3f, 0.75f, 0.3f, 1.0f}, dark[4] = {0.1f, 0.4f, 0.1f, 1.0f};
    for (int j = 0; j < GRID_Z; j++) {
        const float w0 = W_FIRST * powf(W_LAST / W_FIRST, (float)j / GRID_Z);
        const float w1 = W_FIRST * powf(W_LAST / W_FIRST, (float)(j + 1) / GRID_Z);
        for (int i = 0; i < GRID_X; i++) {
            const float x0 = -1000.0f + 2000.0f * i / GRID_X, x1 = -1000.0f + 2000.0f * (i + 1) / GRID_X;
            const float* c = ((i + j) & 1) ? light : dark;
            Ground(v++, x0, w0, c);
            Ground(v++, x1, w0, c);
            Ground(v++, x0, w1, c);
            Ground(v++, x0, w1, c);
            Ground(v++, x1, w0, c);
            Ground(v++, x1, w1, c);
        }
    }
    // The bands: clip x in four strips, all of y, z = -depth at w 1 (the Z depth -z / w).
    for (int k = 0; k < BANDS; k++) {
        const float x0 = -1.0f + 2.0f * k / BANDS, x1 = -1.0f + 2.0f * (k + 1) / BANDS;
        const float xs[6] = {x0, x1, x0, x0, x1, x1}, ys[6] = {-1, -1, 1, 1, -1, 1};
        for (int n = 0; n < 6; n++) {
            v->pos[0] = xs[n];
            v->pos[1] = ys[n];
            v->pos[2] = -band_depth[k];
            v->pos[3] = 1.0f;
            for (int c = 0; c < 4; c++) {
                v->clr[c] = band_colour[k][c];
            }
            v++;
        }
    }
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C3D_RenderTarget* target =
        C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    DVLB_s* dvlb = DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
    shaderProgram_s program;
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    C3D_BindProgram(&program);

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);

    Vertex* vbo = linearAlloc(sizeof(Vertex) * VERTS);
    Fill(vbo);
    GSPGPU_FlushDataCache(vbo, sizeof(Vertex) * VERTS);

    printf("wbuf_test: a W-buffered ground, its depth\n0.0101 (w - 1), and Z-buffered bands\n"
           "(less) at depths:\n");
    for (int k = 0; k < BANDS; k++) {
        const float w_edge = 1.0f + band_depth[k] / (0.01f * FAR / (FAR - NEAR));
        printf("  %.1f: past w %.1f, clip y %.3f\n", band_depth[k], w_edge, GROUND_Y / w_edge);
    }

    u32 frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0x00ffffff);
        C3D_FrameDrawOn(target);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, vbo, sizeof(Vertex), 2, 0x10);
        // The ground: W-buffered, depth = (z / w * -0.01 + 0) * w, less, written.
        C3D_DepthMap(false, -0.01f, 0.0f);
        C3D_DepthTest(true, GPU_LESS, GPU_WRITE_ALL);
        C3D_DrawArrays(GPU_TRIANGLES, 0, GRID_X * GRID_Z * 6);
        // The bands: Z-buffered, depth = -z / w, less, colour only.
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_DepthTest(true, GPU_LESS, GPU_WRITE_COLOR);
        for (int k = 0; k < BANDS; k++) {
            C3D_DrawArrays(GPU_TRIANGLES, GRID_X * GRID_Z * 6 + k * 6, 6);
        }
        C3D_FrameEnd(0);
        frame++;
        if ((frame & 255) == 0) {
            printf("\x1b[8;0Hframe %lu\n", (unsigned long)frame);
        }
    }

    linearFree(vbo);
    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
