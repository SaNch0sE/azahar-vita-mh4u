// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// near_test: what the near plane clips, as a picture to compare. A camera holds at stations that
// bring a cube's front face from well in front of the near plane (0.1) to behind it, the next
// station at each press of A, over a ground plane that runs from behind the camera into the
// distance, a second cube beside the first and a wall with a vertex behind the eye (w through
// zero). A station's picture stays until the next press, so a screen grab of the emulator under
// test and one of a reference (upstream's software renderer) at the same station compare
// directly, whatever speed each runs at.
//
// make MODE=N builds near_test_N.3dsx:
//   0 the near and far planes alone
//   1 the PICA's user clip plane too (GPUREG_FRAGOP_CLIP, coefficients 1, 0, 0, 0.3: clip-space
//     x >= -0.3 w kept)
//   2 back faces culled (counter-clockwise kept): the cube's sides cross the eye's plane at the
//     last stations, and the culling must take the clipped triangles' winding
//   3 front faces culled (clockwise kept)
//   4 the user clip plane as a near cut: everything nearer the eye than 0.8 cut away, as a game
//     hides geometry by the camera; fifteen stations bring the cube's front face from eye z -1.6
//     to -0.2, through the cut
//   5 the other depth layout: citro3d's projection puts the near plane at z = -w and the far at
//     0; mode 5 turns it round (z' = -w - z: near at 0, far at -w) and the depth test with it
//     (less, not greater): the same picture as mode 0. sgxd orients clip z by the layout
//     (ClipZSign): the two modes take its two orientations
//
// The bottom screen names the station and the front face's eye-space z. A: the next station.
// Start quits.

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "program_shbin.h"

#define CLEAR_COLOR 0x203040ff
#define DISPLAY_TRANSFER_FLAGS                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float x, y, z;
    float r, g, b, a;
} Vertex;

#define MAX_VERTS 1536
static Vertex* vbo;
static int verts;

static void Tri(const float a[3], const float b[3], const float c[3], float r, float g, float bl) {
    const float* p[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        vbo[verts++] = (Vertex){p[i][0], p[i][1], p[i][2], r, g, bl, 1.0f};
    }
}

static void Quad(const float a[3], const float b[3], const float c[3], const float d[3], float r,
                 float g, float bl) {
    Tri(a, b, c, r, g, bl);
    Tri(a, c, d, r, g, bl);
}

// An axis-aligned cube of half size h at (cx, cy, cz), each face its own colour.
static void Cube(float cx, float cy, float cz, float h) {
    const float x0 = cx - h, x1 = cx + h, y0 = cy - h, y1 = cy + h, z0 = cz - h, z1 = cz + h;
    const float v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                           {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    Quad(v[4], v[5], v[6], v[7], 1.0f, 0.2f, 0.2f); // front (+z, towards the camera)
    Quad(v[1], v[0], v[3], v[2], 0.2f, 1.0f, 0.2f); // back
    Quad(v[0], v[4], v[7], v[3], 0.2f, 0.2f, 1.0f); // left
    Quad(v[5], v[1], v[2], v[6], 1.0f, 1.0f, 0.2f); // right
    Quad(v[7], v[6], v[2], v[3], 1.0f, 0.2f, 1.0f); // top
    Quad(v[0], v[1], v[5], v[4], 0.2f, 1.0f, 1.0f); // bottom
}

static void Scene(void) {
    verts = 0;
    // The ground, from 10 behind the eye to 60 ahead, a checker of 4x4 tiles in two colours.
    for (int i = -5; i < 5; i++) {
        for (int j = -15; j < 3; j++) {
            const float x0 = i * 4.0f, x1 = x0 + 4.0f, z0 = j * 4.0f, z1 = z0 + 4.0f;
            const float a[3] = {x0, -1.0f, z1}, b[3] = {x1, -1.0f, z1}, c[3] = {x1, -1.0f, z0},
                        d[3] = {x0, -1.0f, z0};
            const int dark = (i + j) & 1;
            Quad(a, b, c, d, dark ? 0.35f : 0.75f, dark ? 0.35f : 0.75f, dark ? 0.30f : 0.65f);
        }
    }
    // The cube the stations walk through, at the origin, and a smaller one beside it, a little
    // further on.
    Cube(0.0f, 0.0f, 0.0f, 0.5f);
    Cube(1.1f, -0.4f, -0.6f, 0.3f);
    // A wall to the left, from 2 behind the eye to 6 ahead: its triangles cross the eye's plane.
    const float w0[3] = {-1.6f, -1.0f, 2.0f}, w1[3] = {-1.6f, -1.0f, -6.0f},
                w2[3] = {-1.6f, 1.5f, -6.0f}, w3[3] = {-1.6f, 1.5f, 2.0f};
    Quad(w0, w1, w2, w3, 0.9f, 0.6f, 0.2f);
}

// The front face's eye-space z at each station: in front of the near plane (-0.1), at it, past it;
// mode 4's through its cut at -0.8.
#if MODE == 4
static const float stations[] = {-1.6f, -1.5f, -1.4f, -1.3f, -1.2f, -1.1f, -1.0f, -0.9f,
                                 -0.8f, -0.7f, -0.6f, -0.5f, -0.4f, -0.3f, -0.2f};
#else
static const float stations[] = {-1.0f, -0.3f, -0.15f, -0.11f, -0.1f,
                                 -0.09f, -0.05f, 0.0f,  0.1f,   0.3f};
#endif
#define STATIONS ((int)(sizeof(stations) / sizeof(stations[0])))

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
    const int u_model_view = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);

    vbo = linearAlloc(sizeof(Vertex) * MAX_VERTS);
    Scene();
    GSPGPU_FlushDataCache(vbo, sizeof(Vertex) * MAX_VERTS);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vbo, sizeof(Vertex), 2, 0x10);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
#if MODE == 2
    C3D_CullFace(GPU_CULL_BACK_CCW);
#elif MODE == 3
    C3D_CullFace(GPU_CULL_FRONT_CCW);
#else
    C3D_CullFace(GPU_CULL_NONE);
#endif
    C3D_DepthTest(true, MODE == 5 ? GPU_LESS : GPU_GREATER, GPU_WRITE_ALL);

    C3D_Mtx projection;
    Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 0.1f, 100.0f,
                  false);
#if MODE == 5
    projection.r[2] = FVec4_Subtract(FVec4_Scale(projection.r[3], -1.0f), projection.r[2]);
#endif
    printf("near_test mode %d: %d triangles, near 0.1\n", MODE, verts / 3);
#if MODE == 4
    // The cut's plane in clip space: z / w at eye distance 0.8, kept on the far side
    // (z - k w >= 0 when z / w grows with distance, else -z + k w >= 0).
    const C3D_FVec at = Mtx_MultiplyFVec4(&projection, FVec4_New(0.0f, 0.0f, -0.8f, 1.0f));
    const C3D_FVec far = Mtx_MultiplyFVec4(&projection, FVec4_New(0.0f, 0.0f, -1.6f, 1.0f));
    const float k = at.z / at.w;
    const float side = far.z / far.w > k ? 1.0f : -1.0f;
    printf("cut: z / w %+.4f, side %+.0f\n", (double)k, (double)side);
#endif

    int station = 0;
    bool shown = false;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        if (hidKeysDown() & KEY_A) {
            station = (station + 1) % STATIONS;
            shown = false;
        }
        // The eye on the z axis, so far from the cube's front face (z 0.5) that the face lies at
        // the station's eye-space z.
        const float eye_z = 0.5f - stations[station];
        C3D_Mtx model_view;
        Mtx_Identity(&model_view);
        Mtx_Translate(&model_view, 0.0f, 0.0f, -eye_z, true);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        // Mode 5 tests less: its depth starts at the far end.
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, MODE == 5 ? 0xffffff : 0);
        C3D_FrameDrawOn(target);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_projection, &projection);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_model_view, &model_view);
#if MODE == 1
        // The user clip plane: kept where 1 x + 0 y + 0 z + 0.3 w >= 0 in clip space.
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP, 1);
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA0, f32tof24(1.0f));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA1, f32tof24(0.0f));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA2, f32tof24(0.0f));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA3, f32tof24(0.3f));
#elif MODE == 4
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP, 1);
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA0, f32tof24(0.0f));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA1, f32tof24(0.0f));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA2, f32tof24(side));
        GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP_DATA3, f32tof24(-side * k));
#endif
        C3D_DrawArrays(GPU_TRIANGLES, 0, verts);
        C3D_FrameEnd(0);

        if (!shown) {
            printf("\x1b[3;0Hstation %d of %d: front face at eye z %+.2f   \n", station + 1,
                   STATIONS, (double)stations[station]);
            shown = true;
        }
    }

    linearFree(vbo);
    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
