<!-- GS point draws on the GPU (Fable plan, 2026-11-15). Model-written plan; estimates unless stated as measured. -->

# Plan: GS point-mode draws off the render thread's per-triangle path ("fix 2")

All paths are under `/home/areki/Documents/Personal/grarakAzahar/azahar/`. Nothing was built or run; this is from reading the code and docs.

## 1. Verdict

**A GPU geometry stage is feasible only as a specialised, recognised-program path, and it attacks the smaller half of the cost. Do a zero-copy feed first, then decide.**

- **A PICA-VS to GXM translator exists and is general.** `Usse::EmitVertexProgram` (`src/video_core/renderer_gxm/usse/vs_usse_gen.h:27-56`, `vs_usse_gen.cpp:2052`) emits USSE directly, with refusals for unbaked bools, loop nesting over two and register budget (`vs_usse_gen.cpp:77-81`, `:533`). It has no EMIT/SETEMIT, one uniform set (`uniforms_f`, 128 rows, `:587`) and one invocation per vertex.
- **A general GS translator is not realistic.** Point mode buffers `gs_inputs / vs_outputs` VS results per GS call (`src/video_core/pica/geometry_pipeline.cpp:53-86`). The GS writes three vertex slots and emits a triangle on `prim_emit`, with an optional winding flip (`src/video_core/pica/shader_unit.cpp:31-47`, `primitive_assembly.cpp:22-24`). A fused program must run the VS N times plus the whole GS per corner, carry two uniform sets, and select the output at the k-th emit. That only works for straight-line GS code and would strain the register budget.
- **The guest vertex stream cannot be used in place.** Each corner needs all N input vertices plus a corner id, so someone still writes an expanded stream on the CPU.
- **The `draw 191 ms` half does not shrink with a GPU GS.** It is `DrawTriangles` -> `BeginDraw` state setup, about 34 us x 5,550 draws. A hardware GS draw pays the same `BeginDraw` (`gxm_rasterizer.cpp:3368`) plus a per-draw vertex uniform reservation (`UploadUniforms`, `:1843-1856`). It also loses the repeat/deferred merge that software draws get (`:2749-2777`).
- **The `feed 139 ms` half is per-triangle overhead, not shading.** `AddTriangle` takes a mutex, allocates, does three 96-byte memcpys and two quaternion tests per triangle (`src/video_core/rasterizer_accelerated.cpp:96-134`), about 1.9 us per triangle.

So the only render-thread saving a GPU GS can deliver is the feed, and a zero-copy feed delivers the same saving with bit-identical output:

- **Phase A (recommended, 1-2 days):** the pre-shade worker writes finished `OutputVertex` triangles straight into the GXM vertex ring; the render thread only queues ranges. The pattern already exists for the emulation thread (`threaded_rasterizer.cpp:270-302`, `:703-706`, `RasterizerGxm::DrawRingRange`, `gxm_rasterizer.cpp:719`).
- **Phase B (the literal fix 2):** only if capture shows it pays. After A it saves core-3 time and nothing on core 1, unless capture shows many consecutive GS draws differ only in vertex data or uniforms.

## 2. Capture first

`gsprof` is no use here: it is skipped when `capture_only` is set (`pica_core.cpp:1768`), which is every pre-shaded draw. Add a `gsdump` flag in `ThreadedRasterizer::PreshadeLoop`, right after `ShadeShippedDraw` (`threaded_rasterizer.cpp:915`), reading `shade_pica`. Trigger it with the existing `dump_now` file convention (`gxm_rasterizer.cpp:3769`).

Per distinct key `(gs hash ^ swizzle hash, gs.main_offset, vs hash)`, write once to `ux0:data/mh4u/gsdump_<hash>.bin`:
- GS and VS code and swizzle, `gs_config` (mode, `fixed_vertex_num_minus_1`, `stride_minus_1`), `variable_primitive`.
- `gs.max_input_attribute_index`, GS input register map, `gs.output_mask`, `vs_outmap_total_minus_1_a`, the rasterizer semantic map, `triangle_topology`.
- GS bool and int uniforms.

Per draw, for two frames each in area 3 and at the quest counter, log:
- key, `num_vertices`, `is_indexed`, output triangle count;
- whether GS float uniforms, VS float uniforms and fragment registers changed since the previous GS draw (fingerprints exist for the memo key, `pica_core.cpp:1808-1809`);
- whether any emit set winding;
- the first draw's input vertices and output triangles, as the reference for an off-console check.

Questions this answers:
- How many GS programs are there? The docs suggest one for village particles (`docs/vita-mh4u-performance.md:203`).
- Is the code straight-line?
- How many VS vertices per primitive, and how many points per draw? 72,100 / 5,550 is about 13 triangles per draw, which fits `docs/plans/area3-gameplay.md:20` (52 HUD draws of six vertices plus one 108-vertex world draw).
- Are GS uniforms constant across a run? This decides whether B could ever merge draws.

Also add to the `timedraw` line (`gxm_rasterizer.cpp:3985`) a counter of GS draws that reached the repeat path against full `BeginDraw`. That is the real size of the 191 ms.

## 3. Steps

### Phase A: zero-copy feed

1. **Ring allocator.** The ring assumes two allocators and protects emulation-thread bytes with `consume_floor` (`src/video_core/vertex_ring.h:45-53`). Give the worker the same treatment: allocate under `ring->mutex`, record the start position in the shipment, and fold it into the floor until the render thread consumes or drops the shipment.
2. **Never block the worker.** On `VertexRing::FULL`, fall back to today's vector path. The worker must not call `RingWaitForSpace`.
3. **`PicaCore::ShadeShippedDraw`** (`pica_core.cpp:1960`): add an overload taking a sink. Shade into the existing vector (it is about 20 KiB), then make one allocation and one copy into the ring, applying the two quaternion flips from `AddTriangle` in the worker. Fill `shipment->ranges`.
4. **`RunSoftwareDraw`** (`threaded_rasterizer.cpp:951-965`): when `ShadeDone && shade_ok && !ranges.empty()`, push the ranges and call `DrawTriangles` once. Add a `QueueRingRange` beside `DrawRingRange`, which draws per range.
5. **Stolen, refused or dropped draws:** pass the range's `end_pos` through the `DropDraw` logic (`gxm_rasterizer.cpp:695-716`) so the space retires.
6. **Half-step, if step 1 looks risky:** keep the vector and add `AddTriangles(span)` on the render thread (one lock, one allocation, one memcpy, flips done by the worker). No third allocator; it still removes most of the feed.

Kill switch: `ux0:data/mh4u/nopreshadering` plus flag `nopreshadering`, read once in `EnablePreshade` (`threaded_rasterizer.cpp:833`). The existing `nopreshade` stays above it.

### Phase B: specialised GPU GS (gated on capture)

1. **Recognition.** A table keyed by GS code hash, swizzle hash, `main_offset`, `gs_config`, input count and output mask, filled from the dump. Each entry is a hand-derived description of how the GS turns one primitive into corners (for a sprite: centre, size, UV rectangle, colour, corner signs, emit order, winding). An unknown key takes the software path.
2. **Program.** New `usse/gs_usse_gen.cpp` reusing `VertexEmitter`: emit the PICA VS body unchanged into temporaries instead of outputs, then the hand-written corner code, selected by a `corner` attribute, then the normal epilogue (`vs_usse_gen.cpp:2052-2088`). Same fragment interface, so the existing fragment programs link. This assumes one VS vertex per primitive. If capture shows N > 1, B needs the VS on the CPU or N attribute sets; I would stop there.
3. **Uniforms.** VS floats stay in `uniforms_f`. Add a `gs_uniforms_f` array holding only the rows the recognised GS reads. Resolve it in `ResolveVertexUniforms`/`VsUniformsOf` (`gxm_rasterizer.cpp:1713-1810`) and write it in `UploadUniforms` from `pica.gs_setup.uniforms.f`, dirty on `gs_setup.uniforms_dirty`. Bake GS bools into the key, as VS bools are (`:2947-2957`).
4. **Vertex data.** In `AccelerateShippedDraw`, add a branch before the `use_gs` decline (`gxm_rasterizer.cpp:3080`) for `use_gs == Yes`, mode Point, recognised key, list topology and an empty primitive assembler. It builds the expanded stream in the ring (4 copies of each point's loader bytes plus a corner byte), using the arena-copy branch (`:3297-3341`) as the template and a static quad index pattern. Pre-shade must skip these draws (`QueuePreshade`, `threaded_rasterizer.cpp:856-858`), so the decision has to be made at ship time from the key table.
5. **Emulation side.** `pica_core.cpp:1162` never offers GS draws to hardware. Either offer recognised ones, or keep `draw.hw` false and let `RunSoftwareDraw` try `real->AccelerateGsDraw(payload)` before falling back.
6. **Guards.** Fall back to software on: unknown key, indexed draw, a winding emit not modelled, `variable_primitive`, `gs.input_to_uniform`, a program refusal, ring full. Kill switch: `ux0:data/mh4u/nohwgs` and flag `nohwgs`. Add a `hwgs_refuse.txt` list like `hwvs_refuse.txt` (`gxm_rasterizer.cpp:2984-2996`).

## 4. Savings and risks

| | Render thread | Worker (core 3) | GPU |
|---|---|---|---|
| A, full | feed 139 ms -> about 5 ms per 3 s: about 1.5 ms/frame at 30 fps | plus one ring copy (about 7 MB/s) | unchanged |
| A, half-step | feed -> perhaps 20-35 ms per 3 s (estimate) | unchanged | unchanged |
| B after A | about zero, possibly negative (uniform buffer per draw, no merge) | VS+GS time removed, but that core already keeps up | 4 vertices per point instead of 6 pass-through; negligible |

- **The 191 ms draw half is untouched by both.** It needs the texture-only-change path listed as a next candidate in `docs/vita-mh4u-performance.md:349` and the menu plan's work on the same sprite draws.
- **Risks, A:** ring accounting with a third allocator (a stall or premature retire shows as corrupt geometry), and a draw that was shaded, then stolen, leaking ring space.
- **Risks, B:**
  - Precision: the software path computes in host float through the JIT, the GPU in USSE float. Sprite edges at half-pixel positions and UVs on texel edges can move by one pixel, which shows on glyphs.
  - A hash matching a program whose uniforms select a different branch.
  - Savestates and the memo are unaffected.

## 5. Verification

- **Phase A must be bit-identical.** Add a diag flag `preshaderingcheck`: the render thread also runs the vector path and memcmps it against the ring bytes, logging mismatches (same shape as `vsmemocheck`, `gxm_rasterizer.cpp:3246-3262`). Run area 3, hub, quest counter and keyboard screen. Compare `dumpframe` captures with and without `nopreshadering` from the same save state. Watch `timedraw` feed/draw, `stolen`/`refused`, ring peak and `stat_dropped_draws`.
- **Phase B, diag flag `hwgscheck`:** for each accelerated draw, also run `ShadeShippedDraw` and compare with the specialised corner maths run on the CPU in C++ (same formula as the emitted program). Log the maximum difference in position, UV and colour per program. Require differences well under a pixel in screen space before enabling.
- **Phase B, off console:** run the emitted program on the `gsdump` reference data, as `vsdump` does (`gxm_rasterizer.cpp:3407-3415`).
- **Phase B, pictures:** `dumpframe` A/B with `nohwgs` on quest counter text, HUD, village particles and the world GS draw in area 3. Diff the images and inspect every differing pixel region.
- **Soak:** 10 minutes of hub and village walk, checking heap and ring peak.

### Critical Files for Implementation
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/threaded_rasterizer.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/pica/pica_core.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/renderer_gxm/gxm_rasterizer.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/vertex_ring.h
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/renderer_gxm/usse/vs_usse_gen.cpp

## Capture results (area 3, diag `gsdump`, 2026-11-15, measured)

600 consecutive GS draws: every one is Point mode with one VS vertex per primitive (`fixed_vertex_num 1`, stride 1), and every point becomes exactly 2 triangles (a quad). So the Phase B precondition (N = 1) holds.

| GS key | Draws | Triangles | Shape |
|---|---|---|---|
| 7d398d1e7297613e | 225 (37%) | 10,814 (90%) | indexed, 1-51 points per draw (world particles) |
| 9e6bb9b64d38d710 | 375 (63%) | 1,216 (10%) | non-indexed, 1-4 points per draw (HUD sprites) |
| a4cb51124f38a2aa | dumped, not in the logged window | | |

GS float uniforms changed in 25 of 600 draws; VS float uniforms in 390 of 600. Dumps: scratchpad `gsd/a3_*`.

## Implementation and result (2026-11-15, measured)

Built as Fable's option (a), a general fused emitter rather than hand-written corner code:
- `Usse::GsStage` / `GsResult` (`usse/vs_usse_gen.h`) and the GS stage in `VertexEmitter` (`ScanGs`, `EmitGsStage`, `EmitGsEmit`). The VS outputs become GS-input quads, the GS reads its own resident `gs_uniforms_f` rows, the k-th EMIT branches to the epilogue for the vertex whose one-hot `gs_corner` has lane k, and END without that emit writes position (0,0,0,1) (culled point). The triangle pattern comes from simulating SETEMIT/EMIT as PrimitiveAssembler does. The analyser refuses loops, bools, address registers, conditional emits, and jumps to anything but END.
- `BuildGsStage` (`gxm_gs_request.h`), shared with the host tool `tools/usse_host/gs_emit.cpp`. On the three area 3 dumps it produces patterns `1 0 2 3 1 2` (particles) and `0 1 2 1 3 2` (HUD), as Fable derived.
- `RasterizerGxm::AccelerateGsDraw`: expands each point's loader bytes once per pattern entry, plus a one-hot corner stream, into the ring, and draws a triangle list. `ThreadedRasterizer` tries it for GS draws before the software path.

On the console it draws correctly: particles and HUD look the same as the software path in timed screenshots. But it does not pay in area 3 (diag `scenetime timedraw`, 3 s windows):

| | GS on GPU | Software (before) |
|---|---|---|
| GPU per frame | 35.2 ms (vertex 5.2) | 33.5 ms (vertex 3.4) |
| Draws per 3 s | 9,898 | 8,700 |
| Software GS shading on the render thread | 205 ms (4,018 small draws) | 340 ms (5,550 draws) |
| Render-thread gxm time (draw setup, uniforms, …) | 28% | 24% |

- **GPU:** +1.7 ms a frame. The VS and GS run per corner (6 vertices a point), on a GPU that area 3 already saturates.
- **Render thread:** saves ~135 ms per 3 s of feeding, then spends about the same on per-draw setup. The software path merges consecutive same-state GS draws into one GPU draw, and the GPU path reserves a uniform buffer per draw.
- **Release camera test:** standing still, shown 24–28 fps against 27–30 without.

So it is **off by default**, opt-in with `ux0:data/mh4u/hwgs` (or diag flag `hwgs`). `hwgs_min.txt` sets the fewest points per draw (default 4).
It could pay only where the render thread is the limit and the GPU has headroom, and only after: 4 vertices a point (indexed), merging consecutive draws with unchanged uniforms, and uniform rows written without re-reserving the whole VS bank. The quest counter's GS draws are mostly 1–4-point sprites the repeat path already merges, so it is not a candidate as is.

Side note: during these runs MH4U menus ("Player Select", "Hunter Search", the Gathering Hall notice) opened on their own, with and without this change. They look like stray touch input on the console, not an emulator bug. They spoil fps runs (GX jumps to ~800 ms/s), so check the end screenshot.
