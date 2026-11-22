// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// clip_test: floods the GPU's clipper with one kind of triangle a build, to find which inputs lock
// up the SGX543's clipper (docs/sgxd/ta.md).
// Each frame draws DRAWS draws of TRIS triangles, made new every frame from a fixed seed, with a
// colour and three texture coordinates each (varyings the clipper interpolates). The position is
// clip space as made here (the PICA keeps -w <= x, y <= w and -w <= z <= 0).
//
// make MODE=N builds clip_test_N.3dsx:
//   0 inside: every triangle inside the view (no clipping; the control)
//   1 near: one or two vertices in front of the near plane
//   2 guard: vertices far past the left, right, top or bottom
//   3 negw: w from -2 to 2, every vertex anywhere (triangles through the eye's plane)
//   4 zero: three vertices at (0, 0, 0, 0) between near-plane triangles (empty stream-out slots)
//   5 gs: mode 1's and inside triangles through a geometry shader that emits about half
//   6 mixed: every kind above, plus vertices at w about the TA's clamp, signed zeros, huge
//     values, infinities and NaNs
//   8 side: one vertex 1.6 to 6 half-views past the right edge only (one guard-band plane)
//   9 corner: one vertex past the right and the top edges at once (two planes)
//   10 quads: quads of corners at +-K w, K 2 to 20 (full-screen passes drawn oversized)
//   11 near2: mode 2's vertices, but within 1.6 to 3 half-views (just past the guard band)
//   12 small: mode 2 with a position and a colour only (no texture coordinates)
//   13 scene: large triangles in eye space around and behind the camera (a ground and walls
//     up to 30 units away, 10 behind), through a perspective projection with near 0.1 and far
//     100, as a game draws them
//   14 scene: mode 13 with near 0.01
//   15 zfree: w 0.2 to 2, z anywhere in +-2 w (past the near and the far plane), x and y inside
//   16 behind: w -2 to 2, x and y inside +-0.9 |w|, z -0.5 w
//   17 flat: w 1 on every vertex, x and y within 12 (2D layers far past the edges)
//   22 negwseed: mode 3's triangles, the generator seeded from the frame number each frame, the
//     frame number logged as "clip frame N" through svcOutputDebugString (azahar's diagnostics
//     build prints Debug.Emulated): a lockup names its frame
//   26 span: mode 22's seeding and log with triangles through one perspective projection (near
//     0.05 to 2, far 20 to 2000 times that), each with a vertex behind the eye, one between the eye
//     and the near plane and one past the far plane: the lockup pair's planes, as a game's
//     projection makes them (the pair's own positions no single projection makes)
//   27 oblique: mode 26's seeding and log with mode 13's kind of scene, vertices behind the eye
//     too, through a projection whose near plane is a random tilted plane in front of the eye (a
//     reflection's oblique projection): clip z is no longer a function of w alone
//   PRECLIP=1 (with any mode): each triangle clipped at w = 1e-5 on the CPU first (its first fan
//     triangle drawn), so none reaches the TA through w = 0; REVZ=1 (mode 26): reversed depth, the
//     near plane at z = -w and the far plane at z = 0
//   28 spanproj: mode 26's triangles in eye space, projected in the vertex program by the
//     frame's projection (its rows as uniforms, as a game draws): sgxd reads the rows to orient
//     clip z; REVZ=1 the reversed-depth projection
//   23 negwreplay: frame FRAME's triangles of mode 22 every frame; with LO and HI, the triangles
//     outside [LO, HI) are replaced by a small one inside the view in the same slot (the cores'
//     runs keep their lengths), to bisect a frame that locks
//   24 negwwindow: mode 22's frames START to STOP as they are, every other frame's triangles the
//     small inside one (the passes and draws the same): which frames a lockup needs
//   25 negwone: triangle T of mode 22's frame FRAME alone in its slot (the others the small one),
//     with T2 (if set) the triangle after it too, GAP slots further on, SWAP to exchange the two;
//     a vertex MODV of the triangle MODT (0 T, 1 T2) changed: w set to MODW, or all four
//     coordinates times MODS (the same projective point at another scale): what in a locking
//     triangle, or pair, locks; FILL, the other slots' triangle: 0 the small inside one, 1 a
//     degenerate one (three equal vertices inside), 2 one wholly past the right edge, 3 a tenth of
//     a pixel at a pixel's corner (no centre covered), 4 one past the near plane at a vertex;
//     REV bit 0/1 reverses T's/T2's vertex order, ROT bit 0/1 starts it at its second vertex
// The bottom screen counts frames. Start quits.

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "program_shbin.h"
#include "small_shbin.h"
#include "proj_shbin.h"

#ifndef MODE
#define MODE 0
#endif
#ifndef FRAME
#define FRAME 0
#endif
#ifndef LO
#define LO 0
#endif
#ifndef START
#define START 0
#endif
#ifndef T
#define T 0
#endif
#ifndef MODV
#define MODV (-1)
#endif
#ifndef MODS
#define MODS 1.0f
#endif
#ifndef T2
#define T2 (-1)
#endif
#ifndef GAP
#define GAP 0
#endif
#ifndef SWAP
#define SWAP 0
#endif
#ifndef MODT
#define MODT 0
#endif
#ifndef FILL
#define FILL 0
#endif
#ifndef REV
#define REV 0
#endif
#ifndef PRECLIP
#define PRECLIP 0
#endif
#ifndef REVZ
#define REVZ 0
#endif
#ifndef ROT
#define ROT 0
#endif
#ifndef STOP
#define STOP 0x7fffffff
#endif
#ifndef HI
#define HI 0x7fffffff
#endif

#define CLEAR_COLOR 0x203040ff
#define DISPLAY_TRANSFER_FLAGS                                                                    \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TRIS 1024
#define DRAWS 4
#define VERTS (TRIS * 3 * DRAWS)

typedef struct {
    float pos[4];
    float clr[4];
    float tc0[2], tc1[2], tc2[2];
} Vertex;

static u32 rng = 0x2545f491;
// Mode 28: the frame's projection, near n and far f, its x and y factors.
static float proj_n = 0.1f, proj_f = 100.0f, proj_fx = 1.0f, proj_fy = 1.0f;

static u32 Next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float Unit(void) {
    return (float)(Next() >> 8) * (1.0f / 16777216.0f);
}

static float Range(float lo, float hi) {
    return lo + (hi - lo) * Unit();
}

static void Inside(float* p) {
    const float w = Range(0.5f, 2.0f);
    p[0] = Range(-0.9f, 0.9f) * w;
    p[1] = Range(-0.9f, 0.9f) * w;
    p[2] = Range(-0.9f, -0.1f) * w;
    p[3] = w;
}

static float Special(void) {
    static const float values[] = {0.0f, -0.0f, 1e-30f, -1e-30f, 1e-5f, 9e-6f, 1.1e-5f, 1e30f,
                                   -1e30f, 1e38f, 0.0f, 1.0f};
    const u32 k = Next() % 14;
    if (k == 12) {
        return INFINITY;
    }
    if (k == 13) {
        return NAN;
    }
    return values[k];
}

// One triangle's three positions, of the kind `kind` (the modes' numbers; 7 the special values).
static void Triangle(int kind, float p[3][4]) {
    for (int v = 0; v < 3; v++) {
        Inside(p[v]);
    }
    switch (kind) {
    case 1: { // near: one or two vertices in front of the near plane (z > 0)
        const int out = 1 + (int)(Next() % 2);
        for (int v = 0; v < out; v++) {
            p[v][2] = Range(0.01f, 3.0f) * p[v][3];
        }
        if (Next() & 1) { // or past the far plane as well
            p[2][2] = -Range(1.01f, 3.0f) * p[2][3];
        }
        break;
    }
    case 2: // guard: far past the sides
        for (int v = 0; v < 3; v++) {
            p[v][0] = Range(-12.0f, 12.0f) * p[v][3];
            p[v][1] = Range(-12.0f, 12.0f) * p[v][3];
        }
        break;
    case 3: // negw: anywhere, w through zero
        for (int v = 0; v < 3; v++) {
            p[v][3] = Range(-2.0f, 2.0f);
            p[v][0] = Range(-2.0f, 2.0f);
            p[v][1] = Range(-2.0f, 2.0f);
            p[v][2] = Range(-2.0f, 2.0f);
        }
        break;
    case 4: // zero: an empty stream-out slot
        memset(p, 0, sizeof(float) * 12);
        break;
    case 8: // side: past the right edge only
        p[0][0] = Range(1.6f, 6.0f) * p[0][3];
        break;
    case 9: // corner: past the right and the top at once
        p[0][0] = Range(1.6f, 6.0f) * p[0][3];
        p[0][1] = Range(1.6f, 6.0f) * p[0][3];
        break;
    case 10: { // quads: half a quad of corners +-K w
        const float k = Range(2.0f, 20.0f);
        const float w = p[0][3];
        const float x[3] = {-k, k, (Next() & 1) ? k : -k};
        const float y[3] = {-k, -k, k};
        for (int v = 0; v < 3; v++) {
            p[v][0] = x[v] * w;
            p[v][1] = y[v] * w;
            p[v][3] = w;
            p[v][2] = -0.5f * w;
        }
        break;
    }
    case 11: // near2: just past the guard band
        for (int v = 0; v < 3; v++) {
            const float s = (Next() & 1) ? 1.0f : -1.0f;
            p[v][0] = s * Range(0.0f, 3.0f) * p[v][3];
            p[v][1] = Range(-3.0f, 3.0f) * p[v][3];
        }
        break;
    case 13:
    case 14: { // scene: eye space through a perspective projection, the PICA's clip space
        const float n = kind == 13 ? 0.1f : 0.01f, f = 100.0f;
        const float fy = 1.0f / tanf(0.5f), fx = fy * 240.0f / 400.0f;
        const float cx = Range(-30.0f, 30.0f), cy = Range(-3.0f, 3.0f), cz = Range(-40.0f, 10.0f);
        for (int v = 0; v < 3; v++) {
            const float xe = cx + Range(-20.0f, 20.0f), ye = cy + Range(-2.0f, 2.0f);
            const float ze = cz + Range(-20.0f, 20.0f);
            p[v][0] = fx * xe;
            p[v][1] = fy * ye;
            p[v][2] = f / (f - n) * (ze + n); // 0 at the near plane, -w at the far
            p[v][3] = -ze;
        }
        break;
    }
    case 26: { // span: through one perspective projection, a vertex behind the eye, one between
               // the eye and the near plane, one past the far plane, in a random order
        const float n = Range(0.05f, 2.0f), f = n * Range(20.0f, 2000.0f);
        const float fy = 1.0f / tanf(Range(0.3f, 0.8f)), fx = fy * 240.0f / 400.0f;
        const float ze[3] = {Range(0.01f, 50.0f), -n * Range(0.01f, 0.99f), -f * Range(1.01f, 3.0f)};
        const u32 first = Next() % 3, step = 1 + Next() % 2;
        for (int k = 0; k < 3; k++) {
            float* const q = p[(first + step * (u32)k) % 3];
            const float reach = ze[k] > 0.0f ? 2.0f * ze[k] + 1.0f : -1.5f * ze[k];
            q[0] = fx * Range(-reach, reach);
            q[1] = fy * Range(-reach, reach);
            q[2] = f / (f - n) * (ze[k] + n); // 0 at the near plane, -w at the far
            q[3] = -ze[k];
            if (REVZ) { // the near plane at -w, the far at 0 (reversed depth)
                q[2] = -q[3] - q[2];
            }
        }
        break;
    }
    case 27: { // oblique: mode 13's scene with vertices behind the eye too, through a projection
               // whose near plane is a tilted plane in front of the eye (a reflection's camera):
               // z = -s (n . eye - d) - k w, the near plane n . eye = d, the far plane where
               // z = -w. A tilted plane leaves points behind the eye on its visible side.
        const float fy = 1.0f / tanf(0.5f), fx = fy * 240.0f / 400.0f;
        float n[3] = {Range(-1.0f, 1.0f), Range(-1.0f, 1.0f), Range(-1.0f, -0.05f)};
        const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float d = Range(0.05f, 2.0f), sc = Range(0.3f, 3.0f), k = Range(0.0f, 0.9f);
        const float cx = Range(-10.0f, 10.0f), cy = Range(-3.0f, 3.0f), cz = Range(-20.0f, 10.0f);
        for (int v = 0; v < 3; v++) {
            const float xe = cx + Range(-20.0f, 20.0f), ye = cy + Range(-3.0f, 3.0f);
            const float ze = cz + Range(-20.0f, 20.0f);
            const float dist = (n[0] * xe + n[1] * ye + n[2] * ze) / len;
            p[v][0] = fx * xe;
            p[v][1] = fy * ye;
            p[v][3] = -ze;
            p[v][2] = -sc * (dist - d) - k * p[v][3];
        }
        break;
    }
    case 28: { // spanproj: mode 26's triangle in eye space (w 1), the frame's projection in the
               // vertex program
        const float n = proj_n, f = proj_f;
        const float ze[3] = {Range(0.01f, 50.0f), -n * Range(0.01f, 0.99f), -f * Range(1.01f, 3.0f)};
        const u32 first = Next() % 3, step = 1 + Next() % 2;
        for (int k = 0; k < 3; k++) {
            float* const q = p[(first + step * (u32)k) % 3];
            const float reach = ze[k] > 0.0f ? 2.0f * ze[k] + 1.0f : -1.5f * ze[k];
            q[0] = Range(-reach, reach);
            q[1] = Range(-reach, reach);
            q[2] = ze[k];
            q[3] = 1.0f;
        }
        break;
    }
    case 15: // zfree
        for (int v = 0; v < 3; v++) {
            p[v][2] = Range(-2.0f, 2.0f) * p[v][3];
        }
        break;
    case 16: // behind
        for (int v = 0; v < 3; v++) {
            const float w = Range(-2.0f, 2.0f);
            p[v][3] = w;
            p[v][0] = Range(-0.9f, 0.9f) * w;
            p[v][1] = Range(-0.9f, 0.9f) * w;
            p[v][2] = -0.5f * w;
        }
        break;
    case 17: // flat
        for (int v = 0; v < 3; v++) {
            p[v][0] = Range(-12.0f, 12.0f);
            p[v][1] = Range(-12.0f, 12.0f);
            p[v][2] = -0.5f;
            p[v][3] = 1.0f;
        }
        break;
    case 7: // special values in some components
        for (int v = 0; v < 3; v++) {
            for (int c = 0; c < 4; c++) {
                if ((Next() & 3) == 0) {
                    p[v][c] = Special();
                }
            }
            if ((Next() & 7) == 0) { // w about the TA's clamp
                p[v][3] = Range(-2e-5f, 2e-5f);
            }
        }
        break;
    default:
        break;
    }
}

static int KindFor(u32 t) {
    switch (MODE) {
    case 22:
    case 23:
    case 24:
    case 25:
        return 3;
    case 26:
        return 26;
    case 28:
        return 28;
    case 27:
        return 27;
    case 4:
        return (t & 1) ? 4 : 1;
    case 5:
        return (t & 1) ? 1 : 0;
    case 6: {
        static const int kinds[] = {0, 1, 1, 2, 3, 4, 4, 7};
        return kinds[Next() % 8];
    }
    case 12:
        return 2;
    default:
        return MODE;
    }
}

// Mode 24: whether this frame's triangles are the generator's.
static bool window_on = true;

// Mode 25: triangles T and T2 of frame FRAME, the changed vertex applied.
static float kept[2][3][4];

static void Triangle(int kind, float p[3][4]);
static void Seed(u32 frame);

static void KeepTriangles(void) {
    Seed(FRAME);
    const int last = T2 >= 0 ? T2 : T;
    for (int t = 0; t <= last; t++) {
        float p[3][4];
        Triangle(3, p);
        // Fill's draws of the colour and texture coordinates, for the generator's sequence.
        for (int k = 0; k < 27; k++) {
            Next();
        }
        if (t == T) {
            memcpy(kept[0], p, sizeof(p));
        }
        if (t == T2) {
            memcpy(kept[1], p, sizeof(p));
        }
    }
    if (MODV >= 0) {
        float (*v)[4] = kept[MODT];
        for (int c = 0; c < 4; c++) {
            v[MODV][c] *= MODS;
        }
#ifdef MODW
        v[MODV][3] = MODW;
#endif
    }
    // REV: bit 0 reverses T's vertex order, bit 1 T2's (the other winding); ROT: bit 0 starts
    // T at its second vertex, bit 1 T2 (the same winding, another first vertex).
    for (int k = 0; k < 2; k++) {
        float q[3][4];
        memcpy(q, kept[k], sizeof(q));
        for (int v = 0; v < 3; v++) {
            const int from = (REV >> k & 1) ? 2 - v : (ROT >> k & 1) ? (v + 1) % 3 : v;
            memcpy(kept[k][v], q[from], sizeof(q[0]));
        }
    }
}

// Modes 22 to 24: the generator's state for frame `frame`.
static void Seed(u32 frame) {
    rng = 0x2545f491u ^ (frame * 0x9e3779b9u);
    if (rng == 0) {
        rng = 1;
    }
    for (int i = 0; i < 8; i++) {
        Next();
    }
}

// PRECLIP: the triangle clipped at w = 1e-5 (the PICA's own w plane) before the GPU sees it, as a
// pre-clip pass would: the polygon's first fan triangle (a second one is left out); nothing left,
// a degenerate triangle. No triangle reaches the TA crossing w = 0.
static void PreClip(float p[3][4]) {
    const float eps = 1e-5f;
    float poly[4][4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const float* a = p[i];
        const float* b = p[(i + 1) % 3];
        const float da = a[3] - eps, db = b[3] - eps;
        if (da >= 0.0f && n < 4) {
            memcpy(poly[n++], a, sizeof(poly[0]));
        }
        if ((da >= 0.0f) != (db >= 0.0f) && n < 4) {
            const float t = da / (da - db);
            for (int c = 0; c < 4; c++) {
                poly[n][c] = a[c] + t * (b[c] - a[c]);
            }
            n++;
        }
    }
    if (n < 3) {
        memset(p, 0, sizeof(float) * 12);
        return;
    }
    memcpy(p, poly, sizeof(float) * 12);
}

static void Fill(Vertex* out) {
    for (u32 t = 0; t < TRIS * DRAWS; t++) {
        float p[3][4];
        Triangle(KindFor(t), p);
#if MODE == 25
        if (t == T) {
            memcpy(p, SWAP ? kept[1] : kept[0], sizeof(float) * 12);
        } else if (T2 >= 0 && t == (u32)(T2 + GAP)) {
            memcpy(p, SWAP ? kept[0] : kept[1], sizeof(float) * 12);
        }
#endif
        if ((MODE == 23 && (t < LO || t >= HI)) || (MODE == 24 && !window_on) ||
            (MODE == 25 && t != T && !(T2 >= 0 && t == (u32)(T2 + GAP)))) {
            static const float small[3][4] = {
                {-0.1f, -0.1f, -0.5f, 1.0f}, {0.1f, -0.1f, -0.5f, 1.0f}, {0.0f, 0.1f, -0.5f, 1.0f}};
            static const float point[3][4] = {
                {0.1f, 0.1f, -0.5f, 1.0f}, {0.1f, 0.1f, -0.5f, 1.0f}, {0.1f, 0.1f, -0.5f, 1.0f}};
            static const float right[3][4] = {
                {3.0f, -0.1f, -0.5f, 1.0f}, {3.2f, -0.1f, -0.5f, 1.0f}, {3.1f, 0.1f, -0.5f, 1.0f}};
            // A tenth of a pixel at a pixel's corner, its centre not covered.
            static const float tiny[3][4] = {{0.0001f, 0.0001f, -0.5f, 1.0f},
                                             {0.0006f, 0.0001f, -0.5f, 1.0f},
                                             {0.0001f, 0.0006f, -0.5f, 1.0f}};
            // Past the near plane at one vertex (w 1 throughout): clipped, no w = 0 crossing.
            static const float nearcut[3][4] = {
                {-0.1f, -0.1f, -0.5f, 1.0f}, {0.1f, -0.1f, -0.5f, 1.0f}, {0.0f, 0.1f, 0.5f, 1.0f}};
            memcpy(p,
                   FILL == 1   ? point
                   : FILL == 2 ? right
                   : FILL == 3 ? tiny
                   : FILL == 4 ? nearcut
                               : small,
                   sizeof(small));
        }
        if (PRECLIP) {
            PreClip(p);
        }
        for (int v = 0; v < 3; v++) {
            Vertex* const x = &out[t * 3 + v];
            memcpy(x->pos, p[v], sizeof(x->pos));
            x->clr[0] = Unit();
            x->clr[1] = Unit();
            x->clr[2] = Unit();
            x->clr[3] = 1.0f;
            x->tc0[0] = Range(-2.0f, 2.0f);
            x->tc0[1] = Range(-2.0f, 2.0f);
            x->tc1[0] = Unit();
            x->tc1[1] = Unit();
            x->tc2[0] = Unit();
            x->tc2[1] = Unit();
        }
    }
}

static C3D_Tex textures[3];

static void MakeTextures(void) {
    for (int i = 0; i < 3; i++) {
        C3D_TexInit(&textures[i], 8, 8, GPU_RGBA8);
        u32 texels[64];
        for (int k = 0; k < 64; k++) {
            texels[k] = ((k ^ (k >> 3)) & 1) ? 0xffffffffu : (0x40c08000u >> (8 * i)) | 0xff;
        }
        C3D_TexUpload(&textures[i], texels);
        C3D_TexSetFilter(&textures[i], GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&textures[i], GPU_REPEAT, GPU_REPEAT);
        C3D_TexBind(i, &textures[i]);
    }
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C3D_RenderTarget* target =
        C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    DVLB_s* dvlb = MODE == 12   ? DVLB_ParseFile((u32*)small_shbin, small_shbin_size)
                   : MODE == 28 ? DVLB_ParseFile((u32*)proj_shbin, proj_shbin_size)
                                : DVLB_ParseFile((u32*)program_shbin, program_shbin_size);
    shaderProgram_s program;
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    if (MODE == 5) {
        shaderProgramSetGsh(&program, &dvlb->DVLE[1], 15); // five outputs of three vertices
    }
    C3D_BindProgram(&program);
    const int u_projection =
        MODE == 28 ? shaderInstanceGetUniformLocation(program.vertexShader, "projection") : -1;

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 2);

    MakeTextures();
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_TEXTURE0, 0);
    C3D_TexEnvFunc(env, C3D_Both, MODE == 12 ? GPU_REPLACE : GPU_MODULATE);
    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE1, 0);
    C3D_TexEnvFunc(env, C3D_Both, MODE == 12 ? GPU_REPLACE : GPU_MODULATE);
    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE2, 0);
    C3D_TexEnvFunc(env, C3D_Both, MODE == 12 ? GPU_REPLACE : GPU_MODULATE);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);

    Vertex* vbo[2];
    for (int i = 0; i < 2; i++) {
        vbo[i] = linearAlloc(sizeof(Vertex) * VERTS);
    }
    printf("clip_test mode %d: %d draws of %d triangles a frame\n", MODE, DRAWS, TRIS);

    u32 frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        Vertex* const v = vbo[frame & 1];
        if (MODE == 22 || MODE == 26 || MODE == 27 || MODE == 28) {
            Seed(frame);
            if (MODE == 28) {
                proj_n = Range(0.05f, 2.0f);
                proj_f = proj_n * Range(20.0f, 2000.0f);
                proj_fy = 1.0f / tanf(Range(0.3f, 0.8f));
                proj_fx = proj_fy * 240.0f / 400.0f;
            }
            char line[32];
            const int n = snprintf(line, sizeof(line), "clip frame %lu", (unsigned long)frame);
            svcOutputDebugString(line, n);
        } else if (MODE == 23) {
            Seed(FRAME);
        } else if (MODE == 25) {
            KeepTriangles();
        } else if (MODE == 24) {
            Seed(frame);
            window_on = frame >= START && frame <= STOP;
            char line[32];
            const int n = snprintf(line, sizeof(line), "clip frame %lu", (unsigned long)frame);
            svcOutputDebugString(line, n);
        }
        Fill(v);
        GSPGPU_FlushDataCache(v, sizeof(Vertex) * VERTS);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target);
        if (MODE == 28) {
            // Standard depth: z 0 at the near plane, -w at the far; REVZ the other way round
            // (z = -w - z), as a reversed-depth projection.
            const float a = proj_f / (proj_f - proj_n), b = a * proj_n;
            C3D_FVec* const rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, u_projection, 4);
            rows[0] = FVec4_New(proj_fx, 0.0f, 0.0f, 0.0f);
            rows[1] = FVec4_New(0.0f, proj_fy, 0.0f, 0.0f);
            rows[2] = REVZ ? FVec4_New(0.0f, 0.0f, 1.0f - a, -b) : FVec4_New(0.0f, 0.0f, a, b);
            rows[3] = FVec4_New(0.0f, 0.0f, -1.0f, 0.0f);
        }
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, v, sizeof(Vertex), 5, 0x43210);
        for (int d = 0; d < DRAWS; d++) {
            C3D_DrawArrays(MODE == 5 ? GPU_GEOMETRY_PRIM : GPU_TRIANGLES, d * TRIS * 3, TRIS * 3);
        }
        C3D_FrameEnd(0);
        frame++;
        if ((frame & 255) == 0) {
            printf("\x1b[2;0Hframe %lu\n", (unsigned long)frame);
        }
    }

    for (int i = 0; i < 2; i++) {
        linearFree(vbo[i]);
    }
    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
