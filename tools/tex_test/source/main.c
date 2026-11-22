// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// tex_test: one texture of every PICA format at every size, each sampled by a small quad every
// frame: the emulator's untile of each (sgxd untiles each once, at its first use).
// The textures hold random bytes, written once.
//
// make MODE=N builds tex_test_N.3dsx:
//   0 one level a texture
//   1 the whole mip chain down to 8x8
//
// The top screen shows a grid: a column a format, a row a size (8 to 512). Start quits.

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "program_shbin.h"

#ifndef MODE
#define MODE 0
#endif

#define CLEAR_COLOR 0x203040ff
#define DISPLAY_TRANSFER_FLAGS                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static const GPU_TEXCOLOR formats[] = {GPU_RGBA8, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4,
                                       GPU_LA8,   GPU_HILO8, GPU_L8,      GPU_A8,     GPU_LA4,
                                       GPU_L4,    GPU_A4,    GPU_ETC1,    GPU_ETC1A4};
static const char* const format_names[] = {"RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4",
                                           "LA8",   "HILO8", "L8",      "A8",     "LA4",
                                           "L4",    "A4",    "ETC1",    "ETC1A4"};
#define FORMATS ((int)(sizeof(formats) / sizeof(formats[0])))
#define SIZES 7 // 8 to 512
#define TEXTURES (FORMATS * SIZES)

typedef struct {
    float x, y, z;
    float u, v;
} Vertex;

static C3D_Tex textures[TEXTURES];
static u32 rng = 0x2545f491;

static u32 Next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
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
    const int u_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);

    // The textures, random bytes: valid texels in every format, ETC1 blocks included.
    int made = 0;
    for (int f = 0; f < FORMATS; f++) {
        for (int s = 0; s < SIZES; s++) {
            C3D_Tex* const t = &textures[f * SIZES + s];
            const u16 side = (u16)(8 << s);
            const bool ok = MODE == 1 && side > 8
                                ? C3D_TexInitMipmap(t, side, side, formats[f])
                                : C3D_TexInit(t, side, side, formats[f]);
            if (!ok) {
                printf("no memory for %s %u\n", format_names[f], side);
                continue;
            }
            u32* const words = (u32*)t->data;
            for (u32 i = 0; i < t->size / 4; i++) {
                words[i] = Next();
            }
            GSPGPU_FlushDataCache(t->data, t->size);
            C3D_TexSetFilter(t, GPU_LINEAR, GPU_NEAREST);
            made++;
        }
    }

    // A 16x16 quad a texture, a column a format and a row a size.
    Vertex* vbo = linearAlloc(sizeof(Vertex) * 6 * TEXTURES);
    for (int i = 0; i < TEXTURES; i++) {
        const float x0 = 8.0f + (float)(i / SIZES) * 27.0f, y0 = 8.0f + (float)(i % SIZES) * 32.0f;
        const float x1 = x0 + 16.0f, y1 = y0 + 16.0f;
        const Vertex q[6] = {{x0, y0, 0.5f, 0, 0}, {x1, y0, 0.5f, 1, 0}, {x1, y1, 0.5f, 1, 1},
                             {x0, y0, 0.5f, 0, 0}, {x1, y1, 0.5f, 1, 1}, {x0, y1, 0.5f, 0, 1}};
        memcpy(&vbo[i * 6], q, sizeof(q));
    }
    GSPGPU_FlushDataCache(vbo, sizeof(Vertex) * 6 * TEXTURES);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vbo, sizeof(Vertex), 2, 0x10);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);

    C3D_Mtx projection;
    Mtx_OrthoTilt(&projection, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    printf("tex_test mode %d: %d textures (%d formats, 8 to 512)\n", MODE, made, FORMATS);

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_projection, &projection);
        for (int i = 0; i < TEXTURES; i++) {
            if (!textures[i].data) {
                continue;
            }
            C3D_TexBind(0, &textures[i]);
            C3D_DrawArrays(GPU_TRIANGLES, i * 6, 6);
        }
        C3D_FrameEnd(0);
    }

    for (int i = 0; i < TEXTURES; i++) {
        if (textures[i].data) {
            C3D_TexDelete(&textures[i]);
        }
    }
    linearFree(vbo);
    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
