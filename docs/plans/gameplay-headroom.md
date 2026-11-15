<!-- Gameplay headroom plan (Fable, 2026-11-15), with the measurements taken since. Estimates unless marked measured. -->

# Gameplay at 30 fps without frameskip: plan

Scope: quest areas and fights in the release build; menus and the quest counter are out of scope for now (owner, 2026-11-15).

## Reframing

Area 3's main target costs 33.3-33.5 ms of GPU a frame against a 33.3 ms budget, and the diag render thread is ~95% busy, ~5 ms a frame of it waiting for the GPU. So the gap is about 3-4 ms of headroom on both the GPU and the render thread, not the 8 ms the alpha-test work was chasing. Options of 1-2 ms that were dismissed earlier now stack.

## Measured (2026-11-15, area 3, slot 2)

GPU per frame, diag `timedraw scenetime`, 50-frame windows:

| Run | Main target | Note |
|---|---|---|
| Baseline | 33.3 ms | |
| `nofog` | 31.9 ms | fog in total: 1.4 ms |
| `dbgconstfog` | 32.3 ms | the fog strip fetch alone: ~1.0 ms |
| `scenetimeord` | scene 0 (bottom-screen clear quad) 1.07 ms; scene 2 (world) 32.3 ms | prices option G1 |

`scenetime` times only scenes closed through `EndScene`; the two blit scenes and the present scene are not in these numbers.

Frame pacing, release, `cam.sh` (camera segments), gameplay seconds only:

| Rule | Shown fps | Seconds at 30+ | Seconds under 29 | Vblanks that armed a skip | Superseded presents |
|---|---|---|---|---|---|
| Old (`oldskip`) | 28.84 | 32 / 83 | 21 | 137 (presents 67, backlog 71) | 8 |
| New, run 1 | 29.42 | 20 / 48 | 2 | 48 (presents only) | 0 |
| New, run 2 | 29.61 | 68 / 111 | 7 | 75 (presents only) | 0 |
| New + G2, run 1 | 30.02 | 105 / 110 | 0 | 1 | 0 |
| New + G2, run 2 | 30.02 | 108 / 112 | 0 | 5 | 0 |

## Findings (Fable)

- About half the skips were false. The backlog estimate is `Depth() * avg_op_us`; under the GX queue the depth holds a whole just-parsed frame, and the average included the present op with its frame-end GPU wait.
- The presents signal fired when one frame was a single vblank late (two pending), then dropped a whole frame.
- The render thread also dropped a fully drawn present as "superseded" whenever the next frame's present was already queued.
- Every sampler in the emitted GXP programs is a dependent read: the non-dependent sampler count is hard-wired to 0 (`usse/gxp_writer.cpp`). That includes the fog strip and textures sampled on raw texture coordinates.
- Every vertex program writes a fixed 27-register output set, whatever the fragment program reads (`usse/vs_usse_gen.cpp`).
- The present scene's `BeginScene` sets `SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY` (`citra_vita/gxm_present.cpp`), which may stop the next frame's tiling from overlapping this frame's fragment work.

## Options, in order

| # | Item | Relieves | Estimate | Status |
|---|---|---|---|---|
| FS | Frameskip: presents signal only when two are pending for 3 vblanks or three are pending; presents out of `avg_op_us`; superseded only with three pending | Pacing | measured +0.6-0.8 shown fps | **Done** (kill switch `oldskip`) |
| G2 | Present scene without the vertex dependency wait | GPU overlap | measured: area 3 holds 30 shown | **Done** (switch `presentvwait`) |
| G1 | Drop the bottom-screen display transfer in Single screen so the clear quad joins the world scene | GPU, render | up to ~1 ms (scene 0 measured at 1.07 ms) | Next (half a day) |
| G3 | Fog strip as a non-dependent (projective iterated) read | GPU | up to ~1 ms (fetch measured) | After a spike |
| G4 | Textures 0-2 on raw coordinates as non-dependent reads | GPU | 0.5-1.5 ms | Same spike as G3 |
| U | Uniform uploads: per-program write list, reachable vertex rows only | Render | 1-1.5 ms a frame (diag) | After G3/G4 |
| T | Timers for the ~12 ms a frame of render-thread time outside the phase timers | Render | measurement | Before U |
| G5 | Vertex output trimming to what the fragment program reads | GPU (vertex/tiler) | 0.3-0.8 ms | Later |
| G6 | Skip the depth store at the frame's last main scene | GPU | 0.2-0.5 ms | Low rank (needs a prediction) |
| G7 | Lighting register moves | GPU | 0.5-1.5 ms | Last (high risk) |

Other gameplay scenes to measure once save states exist: a large-monster fight with effects (roar, dust, fire, hit sparks), two monsters or a full party, caves or night areas (point and spot lights), water or lava areas (procedural texture), other foliage areas, and zone transitions (program emission and upload hitches).
