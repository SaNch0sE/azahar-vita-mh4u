<!-- Fable plan, 2026-11-15. Step 0 was run; see the result at the end. -->

# Area 3 punch-through cost: which model, the test, and the deferred-context plan

Nothing here was run on the device. The libgxm deferred-context rules below are from memory plus the vitasdk header, which declares the calls but documents no restrictions.

## 1. Which model the data supports

**The new numbers do not separate the two models, and they fit a third that neither of us priced.**

- **Model 3: the cost is honest visibility.** `ptoffop` (25.6) is not "the same draws without discard". It turns grass cards into opaque sheets that hide the lit world behind them. Baseline, in-place pre-pass (33.5) and `ppskipb` (31.8) all show the world through the holes. Every new number fits "no waste at all", and then no pre-pass of any kind helps.
- **Model 2 (own overdraw) is weak.** Pass 1 already runs the alpha-only program and nothing moved. The old `dbgminfs minkeep_alpha` run priced trivial punch-through at +1.1 ms, assuming that program is comparable to `AlphaOnlyConfig` (`usse/fs_usse_gen.cpp:2571-2581`).
- **Model 1 (flush of tagged opaque) rests on one old figure.** `ptmin` was 22.4 ms against 18.1 for `ptskip`, about 3 ms the visible world cannot explain. But `ptskip` also dropped the ~1,275 blended alpha-tested draws and the hunter, so that residue is confounded.

So my earlier "6 ms" was overstated: the prize is somewhere between 0 and about 5 ms.

### Step 0 (about an hour): skip the eligible draws

- Add `ptskipop`: `DropDraw()` (`gxm_rasterizer.cpp:698`) for draws passing `PrepassEligible()` (`:3875`). Call the result S.
- S is the world with full hidden-surface removal and nothing hidden by grass, so the true post-fix world cost is at most S.
- S around 25-27: 4-6 ms is being shaded that no correct image needs. Model 1 or 2 is real; go to step 1.
- S at 30 or more: suggestive of model 3, but not proof, since grass depth could still hide more. Step 1 decides.

### Step 1 (half a day to a day): alternate-frame pre-pass, `ppalt`

This uses the still camera (`al.sh`) and no recording. It measures the exact upper bound of pass-1-first and prices pass 1 in isolation.

- **Odd frames:** on the main target, draw only the eligible draws as pass 1 (the `ppskipb` path, `:3933`) and drop every other depth-tested draw. This leaves a grass-only depth buffer.
- **Even frames:** suppress the pending depth clear so the scene force-loads the previous frame's depth (`BeginScene`, `:862-876`; the clear is armed at `gxm_texture_runtime.cpp:731`). Draw the world normally, with eligible draws as pass 2 only (`PrepassSecond`, `:3932`).
- **Reporting:** add frame parity to the `scenetime` key, as `scenetimeord` does (`:1258-1262`).
- Wind sway only makes pass 2's EQUAL miss. If grass vanishes, add the known 1.7 ms back.

| Result | Meaning |
|---|---|
| Even about 25-27, odd about 1-2 | Model 1. Pass-1-first is worth about 4-5 ms. Build section 2. |
| Even about 25-27, odd about 5-6 | Model 2. The cost is the cards' own overdraw even on a trivial program; moving pass 1 gains nothing. |
| Even about 31 | Model 3. The world through the holes is the cost. Stop; see section 3. |

- `sceGxmMidSceneFlush` (`gxm.h:1642`) is not a discriminator: it shades everything tagged in either model.
- The visibility buffer (`gxm.h:1639`) could count pixels, but I don't know whether it counts before or after the kill. `ppalt` answers the question directly.

## 2. Deferred-context plan (only if step 1 says model 1)

### Shape

- **Record only depth-carrying main scenes.** Blits, transfers and the helper scenes at `:583-663` stay on the immediate context.
- **Add `ctx` and `recording` members.** Replace `Device().context` (28 sites) with `ctx` in draw and state paths, and keep the immediate context for Begin/EndScene and Finish.
- **`BeginScene` (`:837`):** `sceGxmBeginScene` on the immediate context, then `sceGxmBeginCommandList(deferred)` and `ctx = deferred`.
  - The existing resets at `:919-932` are exactly the list-start state: `state_cache = {}`, `InvalidateBinding()`, units 3-7 rebound.
  - Assume a list inherits nothing. `SyncState` already sets viewport, clip, cull and depth when the cache is invalid (`:1296-1417`); check that stencil and two-sided state are also set unconditionally.
  - `DrawPendingColourClears` goes into the list, which is fine.
- **`EndScene` (`:1213`):** after `FlushDeferredDraws`, call `sceGxmEndCommandList`, then `sceGxmExecuteCommandList(immediate)`, then the existing `sceGxmEndScene`.
  - Every forced scene end (`:1044`, `:2229`, `:2448`, clears, syncs) already goes through `EndScene`, so mid-scene ends need no extra case.
  - The next scene loads depth and starts its own pre-pass set.

### Pass 1 on the immediate context

- **Order of calls:** in `AccelerateShippedDraw` (`:3399-3421`), record the normal draw first as pass 2 (mode 2, EQUAL, write off, then `PrepassRestore`). Then emit pass 1 on the immediate context. Call order between contexts is free; only Execute's position matters. This keeps the hot path and the dirty-flag consumption in `UploadUniforms` (`:1859-1886`) unchanged.
- **Uncached emission:** there are only about 18-33 such draws a frame, so set everything every time: viewport, clip, cull, the original depth function and write, stencil off, both programs, the textures the alpha-only program samples, fresh uniform reservations with forced writes, and streams (same ring memory).
- **Keeping the caches truthful:** bundle the shadows that `BindPrograms` and `UploadUniforms` mutate and swap in a scratch bundle for pass 1. That covers `state_cache`, the pipeline binding, `uniform_pipeline`, `uniform_scene`, `last_fs`, bound-texture shadows and the `begin_key`/`begin_epoch` repeat state (`:2566-2571`). The recorded side then never sees pass 1.
- **`FlushDeferredDraws` (`:2745`)** needs nothing, because merges belong to the recorded stream.
- **After Execute,** treat immediate state as undefined. Nothing is cached there, so nothing to fix.

### Memory

- `SceGxmDeferredContextParams` (`gxm.h:1244-1257`) takes host memory, initial VDM, vertex and fragment buffers, and three grow callbacks of the form `(args, requestedSize, *size)`.
- Back them with a bump arena per scene. The callbacks return the next chunk, or null to fail the reserve; on failure, end the list, execute it, end the scene and continue.
- List memory must outlive the GPU's use of it. Rotate three arenas and retire them on the existing epoch list (`ring_retire`, just after `:1262`).
- Sizes: measure the main scene's peak first. My guess is 256 KB VDM, 2 MB vertex and 1 MB fragment per arena, about 10 MB of GPU-mapped memory.
- The immediate rings are 16 MB and 8 MB (`gxm_present.cpp:1204-1205`) and could shrink to pay for it.

### Not for the deferred context (unverified)

- Begin/EndScene, MidSceneFlush, Finish, SetVisibilityBuffer, notifications.
- Guard `sceGxmSetUserMarker` (`:1728`) and the diag `scenetime` Finish calls.
- Uniform reserves work there, through the callbacks.

### When moving pass 1 is not equivalent

For LESS-type opaque depth writers the final depth is a minimum, so it is order-independent, and the colour pass stays in place. It breaks in these cases:

1. **A depth clear or an ALWAYS depth write after scene start.** The pre-written depth is wiped and pass 2 misses.
2. **Stencil.** An earlier draw's depth pass or fail changes what it writes.
3. **Mixed depth directions,** or EQUAL-tested draws earlier in the scene.
4. **A blended, depth-writing draw K earlier in the scene.** A draw X behind the grass is now rejected, so K blends over something else.
5. **Coplanar geometry.** An earlier draw at exactly the grass depth loses under LESS where it used to win. This is undetectable, so it needs a magenta check mode and a kill switch.
6. **A render-target or depth-surface change.** That is a new scene, and the set resets.

**Eligibility:** `PrepassEligible` plus per-scene latches. Once a scene has seen any of cases 1-4 or a second depth direction, later grass draws fall back to a plain single draw.

### Effort, risk and milestones

- **Effort:** 6-9 days. **Risk:** high. Deferred contexts are untested here, the render thread is at about 94%, and there is the coplanar case.
- **M0:** `ptskipop` and `ppalt`, one day. This proves the gain before anything is built.
- **M1:** record the main scene with no pre-pass, 2-3 days. Pass means an identical picture, equal `scenetime` and equal render-thread time.
- **M2:** pass 1 on the immediate context.
- **Fallback if deferred contexts misbehave:** our own software draw list replayed at `EndScene`. It needs no libgxm unknowns but copies the uniforms, roughly 1-2 ms of render thread.

## 3. If step 1 says model 3 or model 2

Stop the pre-pass work and remove `prepass`. What is left for area 3's GPU time is cost per visible fragment:

- **Fog, procedural texture and the rest:** 3.7 ms in the area-3 plan's table.
- **Lighting register moves** (option L in that plan): 1-2 ms.
- **Per-vertex lighting or fog on foliage only:** a visible trade-off, off by default.
- **Under model 2 specifically:** the only lever is fewer punch-through fragments, by drawing eligible cards front to back within consecutive runs. That is order-dependent, and I would not ship it.

Past that, accept about 28-29 shown fps here and spend on frame pacing (FS1, FS2) and the render thread.

### Critical Files for Implementation
- src/video_core/renderer_gxm/gxm_rasterizer.cpp
- src/video_core/renderer_gxm/gxm_rasterizer.h
- src/video_core/renderer_gxm/gxm_alpha_prepass.cpp
- src/citra_vita/gxm_present.cpp
- src/video_core/renderer_gxm/gxm_texture_runtime.cpp
- Header reference: $VITASDK/arm-vita-eabi/include/psp2/gxm.h

## Step 0 result (2026-11-15, measured)

`ptskipop` (the ~1,530 eligible draws per 3 s dropped, world drawn with full hidden-surface removal): **30.3 ms** a frame on the main target, against 33.5 baseline. That is in the "30 or more" row: the cost is mostly the world seen through the grass holes (model 3). Even a perfect scene-start pre-pass could recover at most ~3 ms, less pass 1 and the 1.7 ms colour pass, so about 1-2 ms. The deferred-context build is not worth 6-9 days at high risk, so it was not started. `prepass` stays as an opt-in diag experiment.
