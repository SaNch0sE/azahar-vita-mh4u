# Strip runs: repeat indexed-strip draws share one draw's setup

Date: 2026-11-26. Branch mh4u-beta1 (sgxd renderer, PS Vita).

## Intent

MHXX (and likely MH4U, same engine) drops frames in effect-heavy moments because the render
thread runs out of CPU time; the GPU has headroom. Part of the render thread's per-second work
is draws that repeat the draw before them exactly except for which index list they read. Each
one pays the whole `RasterizerSgxd::Draw` setup again (~43 us). This change lets those draws
reuse the setup of the draw they follow, so the render thread does less work per frame with
the GPU work and the picture unchanged.

Success: less render-thread time per second in the measured scenes, the same picture, no new
stalls, crashes or asserts, in MHXX and MH4U. Non-goals: the geometry-shader (particle) draws
(a later project, waiting on a clash measurement), GPU-side savings, any change in what the
GPU draws. Anti-goal: any visible difference, or a gain bought with a rare corruption.

## Measured facts (tools build, MHXX slot 1, calm seconds; commits 5f1cfb301, 445783a99, acf860187)

- ~8000 draws/s: ~2550 geometry draws, ~5400 others; ~5100 of the others are indexed strips.
- ~1450/s of the others repeat the draw before them: same registers (but 0x200-0x23f), same
  programs, same bool/int and float uniforms.
- Every repeat is an indexed triangle strip, 16-bit indices, differing from the draw before
  only in registers 0x227 (index list address/format) and 0x228 (vertex count). Strips are long
  (~380 vertices avg); none has 6 or fewer vertices.
- On the parse side, with the checks `RenderThread::MergeDraw` uses (dirty registers but the two
  allowed, no uniform upload, no program/swizzle upload, no LUT/fog/proctex table change, no
  default attribute change), ~1400/s join a run; ~700 runs/s. None is refused by a uniform upload.
- A repeat draw's render-thread time today: uniforms 2, textures 2, pass 3, vertex stage 25,
  fragment 5, submit 5 us (~43 us).
- In `VertexStageFor`, an indexed draw's fetch addresses do not depend on the indices
  (`first = 0`); the indices only give (a) `indices_gpu` (the guest list read in place for 16-bit
  lists), (b) the vertex range `vertex_min/max`, used only to clean the CPU cache for the vertex
  bytes read, and (c) the index list's own cache cleaning. Nothing after the vertex stage in
  `Draw` reads the vertex range.

Expected saving: ~1400 draws/s x ~35 us = ~50 ms/s of render-thread time in calm seconds
(~8% of its work). The share in effect clashes is not measured yet.

## Approaches

1. **Parse-side strip runs, render-side shared setup (chosen).** The parse holds an indexed
   strip like it holds a geometry point run today (`HeldRun`, `MergeDraw`, `FlushRun`), and
   joins the following strips whose only changed registers are 0x227/0x228. The run goes to the
   render thread as one record listing each draw's index register, count and scanned range. The
   render side runs the first draw through `Draw` as today; each further draw reuses that draw's
   finished submission (`SgxdRecipeDraw` + `SgxdRecipeDrawPrograms`) with its own index address
   and count, after cleaning the cache for its own index list and vertex range. The GPU gets
   exactly the draws it gets today, in the same order, with the same state blocks (which the
   existing SA/PDS caches already share between such draws).
2. Concatenate a run's index lists into one stitched strip (degenerate triangles between
   strips), drawn once. Saves the per-draw submit too, but copies ~1 MB/s of indices into pass
   data and changes the GPU's triangles (degenerate joins, winding parity). Rejected: it changes
   what the GPU draws, and the extra saving is small (~5 us/draw).
3. A render-side fast path only (detect a repeat in `Draw` and skip work). No parse change, but
   it has to prove "nothing changed" from the render side, where register writes have already
   been applied, and it keeps the parse side's per-draw work. Rejected: the parse already has the
   exact "only these registers changed" test, and holding runs there is an existing pattern.

## The design (approach 1)

Parse side (`render_thread.cpp`):
- A held strip run beside the geometry run: started by an indexed, non-geometry,
  triangle-strip draw; joined by the next such draw when MergeDraw's conditions hold with 0x227
  and 0x228 as the allowed registers. Any other record (`ParsedBegin`) sends it first, as the
  geometry run is sent.
- A run of one draw is sent as the plain `Op::Draw` it is today. A run of two or more is one new
  record: each draw's index register value, count and its `DrawIndexRange` (scanned lo/hi and
  cleaned bits). A cap on draws per run keeps the record small.
- Only 16-bit index lists join (8-bit ones are widened into new pass data per draw; none are
  measured joinable anyway).

Render side:
- The first draw: registers as the run's first draw had them, through `AccelerateDrawBatch` as
  today. `Draw` reports whether its submission is shareable and keeps it. Shareable means the
  draw reached `backend.Draw` (not `DrawKeepingAlpha`) on the plain path: no own-pixel units, no
  W-depth fit or Z-on-W mapping, no immediate mode, no geometry stage, and the pass it drew into
  is still open (same `DataOpens`/pass mark).
- Each further draw: if the kept submission is still shareable and the pass has room
  (`PassDraws`, `DataLeft`), clean the cache for its index list and vertex range exactly as
  `VertexStageFor` does (using its scanned range), widen `pass_drawn` as `Draw` does, and call
  `backend.Draw` with the kept submission, its own index address and count. Otherwise set its
  registers and run it through `AccelerateDrawBatch` as today. The last draw's registers are left
  as the guest had them.
- Everything the per-draw counters, tools and diagnostics hooks count per draw keeps counting.

Verification built in:
- A diagnostics-build check that runs each shared draw's full path too (on a copy of the state
  it needs, or by comparing the submission `Draw` would build) and asserts the kept submission
  equals it in every field but the index address and count. Run on MHXX and MH4U scenes.
- Tools-build counters: draws shared, runs, draws that fell back to the full path and why.
- Measured A/B with the existing scripts: render-thread ms/s and fps per second, MHXX slot 1 and
  an MH4U slot, the same build with the run on and off.

## Decisions from the design review (builder check)

- **Self-reads (the first draw changes renderer state).** A run is shareable only when none of
  the first draw's sampled texture units overlaps the pass's colour or depth buffer, and no
  depth write-back or owned-picture write-back ran for it. Otherwise every draw of the run takes
  the full path, which keeps today's pass ends and write-backs. Decided at the first draw, so a
  refraction-style strip that samples its own target can never read stale pixels.
- **Short strips.** Only strips of 7 or more vertices join: the quad heuristics (`OwnPixelUnits`,
  `CoversTarget`, `FillsClearQuad`, the 4/6-vertex `TextureReads` narrowing, `MapZOnW`) look at
  vertices only for draws of 6 or fewer. Measured: no joinable strip has 6 or fewer.
- **Index ranges.** A later draw shares only when its scanned range came with the record
  (`scanned != 0`) and its list lies in guest memory; otherwise it takes the full path. The
  cache cleaning for a draw's index list and vertex range moves out of `VertexStageFor` into one
  function both paths call, so they cannot drift apart.
- **Diagnostics hooks.** A shared draw steps `draw_serial`, calls `VprofDraw` and honours
  `ReplaySkipsDraw` (a skipped shared draw is not submitted), and counts in `ToolsMergeNote` and
  the per-pass tallies like any draw, so draw numbers match between run-on and run-off builds.
- **Holding draws back.** A run holds at most 16 draws, then is sent. The render thread's
  "asleep for records" time is part of the A/B measurement, to show the hold adds no idle time.

- **Which games.** Every game, not behind a title tuning (the user's decision: it changes no
  picture by design). The A/B toggles it with a tools/diagnostics sgxd flag (`nostripruns`).

## Constraints

- One commit per fix/feature with its measurement; never lower the render resolution.
- The user's settings untouched by tests; one console run at a time; close the app after.
- Nothing personal in commits.
- Must not change GPU output: the GPU must receive the same draws in the same order.

## Left to the builder

- The record's exact layout and the run cap.
- How `Draw` hands back its shareable submission (members vs a returned struct).
- How the diagnostics equality check gets the full-path submission without side effects.
