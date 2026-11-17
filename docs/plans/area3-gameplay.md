# Area 3 gameplay: plan to hold 30 shown fps in quest areas

Status: plan only, not started (2026-11-14). Code claims were checked against `mh4u-vita-fixes` at 2cb311511. Gains are estimates until the Phase 0 runs are in; nothing here was run on the console.

Scope: MH4U quest areas on the Vita release build, measured at save state slot 2 (area 3). Companion to `docs/plans/menu-rendering.md`; section 8 says what to share with it.

## 1. Conclusion

- **The GPU is the limit, and the alpha test's position in the fragment program is the likely cause.** The emitter puts the alpha kill after lighting and every TEV stage, so the whole lit program is the part before the kill. The source's own scissor comment says that part runs for every fragment, visible or not.
- **Recommended order:**
  1. Phase 0: half a day of measurement. Two zero-code GPU runs, an alpha-test census, a one-hour blend experiment, and release counters for frameskip and GPU wait.
  2. Phase 1: early alpha kill in the fragment emitter. Expected 5-9 ms of the 33.5 ms GPU frame; exact by construction; medium risk because it touches the emitter.
  3. Phase 2: render-thread trims, starting with the menu plan's Phase 1 (F0 + texture-only path), which applies here unchanged.
  4. Phase 3: only if still short. Static alpha resolve, frameskip policy, the residual bottom-screen scene.
- **Frameskip is mostly a symptom.** It drops about what a pipeline at ~29 fps capacity must drop. One of its two active signals is noisy and may over-skip by 0-1 fps; Phase 0 counts it before anything is changed.
- **Do not pull in the menu plan's Phase 2 (GX trims) or Phase 3 (world skip) here.** The GX thread is at 300-480 ms/s in area 3 and is not a limit.

## 2. Findings that change how the measurements read

1. **The "particle" geometry-shader draws are the HUD.** In the dump, each frame has 188 fully set-up draws. Of the 53 GS ones, 52 come after the post-processing quads with depth test and write off, six vertices each, rotating through 512x512, 256x512 and 64x256 textures. Only one GS draw per frame (108 vertices, lit, fogged, depth-tested) is in the world.
2. **The dump lists only draws that paid a full setup on the software path.** The ~57 software draws per frame that repeat or merge are not in it. So the HUD is ~110 GS draws per frame, about half of them full setups (4,590 key misses per 3 s = 54 per frame).
3. **The alpha kill is the last thing before fog.** `FragmentEmitter::Emit` (`src/video_core/renderer_gxm/usse/fs_usse_gen.cpp`) runs `Scissor`, `SampleTextures`, `ProcTex`, `RoundPrimary`, `Lighting`, all six `TevStage` calls, then `AlphaTest`, then `Fog` and the pack. Only fog and the pack are after the kill.
4. **Temporaries already survive the kill.** `Fog` and `PackF16` read `TmpOutput` after `NewPhaseAfterKill`, and W-buffer programs do the same. Moving the kill earlier needs no recomputation after it and no new instruction form.
5. **Scissor-first programs are the precedent for a long second part.** When only the scissor test kills, `Scissor()` kills first and sampling, lighting and TEV all run after it. The source comments say all 148 of SM3DL's programs have that shape.
6. **World draws have blending enabled with One/Zero factors.** Earlier `logblend` captures (deck, select, hub) show lit draws as `eq 0/0 src 1/1 dst 0/0`. `BindPrograms` passes that to GXM as ADD/ONE/ZERO rather than FUNC_NONE. Whether libgxm treats it as opaque is unknown; area 3 was not captured.
7. **World alpha comes from simple sources.** Stage 0 words in the dump: `0EEE06E4` (alpha = constant) on 61 draws per frame, `0EE406E4` (alpha from texture 1) on 29, `0E0E06E4` (alpha involves vertex colour) on 26. Later stages were not logged.
8. **The world is ~118 lit hardware draws and ~41k vertices per frame.** 46 of them repeat the previous draw's fragment registers; 21 more differ only in texture 1's address.
9. **Skip signals in diag are split between two causes.** Per 300 vblanks: presents 5-9, backlog 6-12, total skips 11-19. The backlog signal is `Depth() * avg_op_us >= 120 ms` (`gpu.cpp`, `gpu_thread.h`).
10. **`avg_op_us` includes the present op.** That op contains the frame-end GPU wait (5 ms average here). One such sample lifts the 15/16 moving average by 300 us or more for the next few dozen ops. Under the GX queue, `Depth()` at the vblank already holds the whole frame just parsed; the code turns the depth signal off for that reason but not the time estimate.
11. **The release "render ~N ms" figure is noisy for the same reason.** It multiplies one op in eight by eight, and a sampled present op counts its GPU wait eight times.
12. **The program disk cache version string is truncated.** `VersionBytes` is 48 and the string is 32 + 1 + 16 characters, so a suffix would be cut off. A release kill switch that changes emitted programs must go into `fs_hash` instead, as the debug-output bits already do.
13. **A residual bottom-screen scene remains.** Each frame has one clear quad on the main target followed by a 240x320 blit, as separate scenes, although the bottom screen's draws are dropped in `PicaCore::DrawArrays`.

## 3. Where the frame goes

GPU, main target, per frame (from the scenetime logs; fragment = total minus ~3.4 ms vertex):

| Programs | Total | Fragment |
|---|---|---|
| Vertex colour only | 7.0 ms | 3.7 ms |
| + textures | 10.7 | 7.4 |
| + TEV combiner | 14.5 | 11.2 |
| + lighting | 21.5 | 18.2 |
| + alpha test | 29.6 | 26.2 |
| Full (fog, proctex, rest) | 33.3 | 29.9 |

The alpha test adds 45% to fragment time while its own instructions are about 3% of a lit program. On the deck it added 0.3 ms.

Render thread, diag, per frame (85 frames per 3 s, 95% busy):

| Part | Per frame |
|---|---|
| Waiting for the GPU at frame end | ~5.4 ms |
| Program binding, of which uniform uploads ~3.0 ms (21.6 us each) | ~5.6 ms |
| Draw, textures, framebuffer, invalidate, state, scene | ~7.6 ms |
| Outside the timers (shipments, pre-shade feed, layout, queue) | ~12.4 ms |

The 110 HUD draws cost ~91 us each by the "software vertex shading" figure, about 10 ms per frame in diag. That figure overlaps the rows above.

## 4. Ranked options

| # | Option | Relieves | Expected gain | Risk | Effort | Verdict |
|---|---|---|---|---|---|---|
| K | Early alpha kill: compute the alpha chain first, kill, then light and combine | GPU | 5-9 ms per frame | Medium (emitter) | 2-3 days | Phase 1 |
| E0 | Pass ADD/ONE/ZERO blending as FUNC_NONE | GPU | 0, or large if libgxm treats it as translucent | Very low (exact) | 1 hour | Phase 0 experiment |
| F0+A | Menu plan Phase 1: uniform dirtiness fix and texture-only path | Render | ~1-1.5 ms per frame in diag (up to 27 of 54 HUD setups) | Low | 1-2 days, shared | Phase 2 |
| U | Uniform upload trims (vertex bank extent, fragment write plan) | Render | ~1-1.5 ms per frame in diag | Low-medium | 1-2 days | Phase 2 |
| R | Static alpha resolve: alpha-tested draws whose alpha is provably constant bind the no-kill program | GPU | Unknown until the census; large if opaque geometry is drawn with the test on | Low-medium | 1-2 days | Phase 3, by census |
| FS1 | Keep present ops out of `avg_op_us`; count only older frames' ops under the GX queue | Pacing | 0-1 shown fps | Low | Half a day | Phase 3, by counters |
| FS2 | No second consecutive skipped frame unless three presents are pending | Pacing | 0-1 shown fps on dips | Low | Half a day | Phase 3, by counters |
| S | Drop the bottom-screen display transfer in Single screen so its clear quad joins the world scene | GPU, render | ~0.5-1.5 ms GPU | Low | Half a day | Phase 3, price first with `scenetimeord` |
| A2 | Extend A to TEV-only changes (HUD alternates two programs) | Render | ~0.5-1 ms per frame beyond A | Medium | 2-3 days | Park; decide from A's residual counters |
| O | Per-surface "all texels opaque" verdict feeding R | GPU | Extends R to textures with an alpha channel | Medium | 3-4 days | Park; share the verdict with menu plan 6.3 |
| L | Lighting register-move reduction (new allocator) | GPU | 1-2 ms | High (miscompile forms) | 1 week+ | Reject for now; K cuts how many fragments run lighting |
| V | Per-vertex lighting or fog | GPU | 3-7 ms | Not visually identical on large polygons | Medium | Reject |
| Z | Reorder opaque before alpha-tested draws | GPU | Unknown | Order-dependent results at equal depth | High | Reject |
| X | Lower resolution, scene merges beyond S | GPU | - | Target is already native 240x400 | - | Reject |

## 5. Phase 0: measurement (no behaviour change)

Use slot 2 through `ux0:data/mh4u/autoload.txt`. The scripts are the local test scripts `al.sh` (still camera) and `cam.sh` (camera segments), kept outside the repository.

### 5.1 Two GPU runs with existing flags

Run diag with `timedraw scenetime` plus:

1. `dbgminfs minkeep_tex minkeep_tev minkeep_alpha`. Compare with 14.5 ms (textures + TEV).
2. `dbgminfs minkeep_alpha`. Compare with 7.0 ms.

Reading:
- If run 1 adds ~5 ms or less (against 8.1 ms on top of lighting), the cost follows the length of the part before the kill. K then recovers most of it.
- If run 1 adds ~8 ms, the cost is fixed per alpha-tested fragment. K then saves only the fragments the test rejects; move R up.
- Run 2 prices the fixed overhead with almost nothing before the kill.

### 5.2 Alpha-test census (diag, under `timedraw`)

- File: `src/video_core/renderer_gxm/gxm_rasterizer.cpp`, in `BindPrograms` after the two existing normalisations, printed in `EndFrame` next to the "repeat misses" line.
- Count per window, for draws whose program still has an alpha kill:
  - draws and vertices, split hardware and software;
  - the top six (function, reference) pairs;
  - which inputs the alpha chain reads: texture 0/1/2, vertex colour, constant, lighting colour, procedural texture, combiner buffer, or a colour lane of the previous stage;
  - the format of each texture in the chain, as "has alpha channel" or not;
  - depth write on or off, and blend One/Zero or other;
  - eligible for K or not, with the reason (uses the analysis function of 6.2, written first).
- Also print total world vertices, so the alpha-tested share is visible.

### 5.3 E0: opaque blend canonicalisation

- File: `gxm_rasterizer.cpp`, `BindPrograms`, after the RGB8 destination-alpha resolution.
- If colour and alpha are both (ADD, ONE, ZERO), set both functions to `SCE_GXM_BLEND_FUNC_NONE`. The result is the same value by definition.
- Gate it behind a diag flag `blendcanon` for the test. Compare scenetime with and without.
- If it gains anything, make it default-on with `noblendcanon` (diag) and `ux0:data/mh4u/noblendcanon` (release). It changes only the pipeline key, not `fs_hash`.

### 5.4 Release counters

- `src/video_core/gpu.cpp`, `VBlankGpuWork`: count per second which signal armed a skip (presents, backlog) and how many skipped frames directly followed a skipped frame. Atomics in `Common::PipelineStats`.
- `src/video_core/gpu_thread.cpp`, worker loop: always time ops with `swap_marker`; keep one-in-eight for the rest. Report busy time as eight times the sampled non-present time plus the exact present time.
- `gxm_rasterizer.cpp`, `EndFrame`: time `runtime.WaitForEpoch` with `NowUs` in release too (two clock reads per frame).
- `src/citra_vita/citra_vita_main.cpp`, the release `[mh4u] speed` line: append `gpuwait N ms  skip pN bN rN`.

### 5.5 Baseline to record

- Release: `cam.sh` output and 30 s still.
- Diag: `timedraw`, then `timedraw scenetime`, then `timedraw scenetime scenetimeord` (prices option S).
- Regression scenes for later comparison: deck both views, ship cutscene, village walk, character select, title screen.

## 6. Phase 1: early alpha kill (K)

### 6.1 What changes

For eligible configurations the program becomes:

1. scissor value, texture samples, vertex colour rounding (as now);
2. the TEV chain's alpha only, the alpha test, the kill;
3. procedural texture, lighting, the full TEV chain, fog, pack (unchanged code).

One kill and two parts, as now. No new inputs, uniforms or samplers, so `PlanInputs`, `PlanUniforms`, `gxp_writer.cpp` and `UploadUniforms` do not change. Step 3 recomputes alpha with today's code, so what is written to the target is today's value; only the kill decision comes from step 2.

### 6.2 Eligibility (from `FSConfig` only)

Add to `fs_usse_gen.h` / `.cpp`:

```
struct AlphaChainInfo { bool eligible; u8 reason; u16 inputs; };
AlphaChainInfo AnalyseAlphaChain(const Pica::Shader::FSConfig&);
void SetEarlyKill(bool on);  void SetEarlyKillCheck(bool on);  void SetEarlyKillAll(bool on);
bool EarlyKillEnabled();
```

Eligible when all hold:
- the alpha function is neither Always nor Never;
- no W-buffer depth write (`uses_depth`), in v1;
- lighting is on, in v1 (that is where the cost is; `earlykillall` widens it for testing);
- no debug stop stage or debug output is active for the program;
- for every stage that is not a pass-through:
  - `color_op` is not Dot3_RGBA, and `alpha_op` is one of Replace, Modulate, Add, AddSigned, Subtract, Lerp, MultiplyThenAdd, AddThenMultiply;
  - every alpha source the operation reads (use `ReadSources` and `AlphaSource`, which handle unused arguments and the stage-0 Previous quirk) is PrimaryColor, Texture0-2, Constant or Previous;
  - a Previous source uses the SourceAlpha or OneMinusSourceAlpha modifier only (other modifiers read a colour lane of the previous stage);
  - Texture0 is allowed only when its type is 2D.

Not eligible in v1: lighting colours, the procedural texture, and PreviousBuffer as alpha sources. The census reports how often each reason occurs; widen only if it matters.

### 6.3 Emitter changes (`fs_usse_gen.cpp`)

1. **`AlphaOnlyStage(stage)`**: returns a copy whose colour half mirrors its alpha half, so the existing four-wide code computes the alpha value in every lane with forms already in use.
   - colour sources 1-3 = alpha sources 1-3;
   - colour modifier from alpha modifier: 0/1 -> 2/3 (alpha), 2/3 -> 4/5 (red), 4/5 -> 8/9 (green), 6/7 -> 0xC/0xD (blue);
   - `color_op = alpha_op`; colour scale = alpha scale.
2. **`TevStage(u32 index, const TevStageConfig* override = nullptr)`**: with an override, use it instead of `config.texture.tev_stages[index]` and skip the snapshot copy at the end. Everything else, including the clamp and the byte rounding at `last_active_stage`, is the same code.
3. **`AlphaPrologue()`**:
   - zero `TmpOutput` with the same two adds the body uses;
   - for each stage up to `last_active_stage`, skip pass-through stages; skip a stage whose alpha half is the identity (Replace of Previous with SourceAlpha, scale 1, index > 0) unless it is `last_active_stage`; otherwise call `TevStage(i, &AlphaOnlyStage(stage))`;
   - under `tevround`, skip nothing.
4. **`Emit()`**, when eligible and enabled:
   - compute `last_active_stage` and call `PlanCombinerBuffer()` before the prologue;
   - order: `Scissor`, `SampleTextures`, `RoundPrimary`, the texture-0 zero copy, `AlphaPrologue`, `AlphaTest` (unchanged: reads `TmpOutput + 2` lane Y, folds the scissor, kills, opens the next part), then `ProcTex`, `Lighting`, the existing zeroing and stage loop, no second `AlphaTest`, `Fog`, pack.
5. **Why the temporaries are safe.** Lighting reuses `TmpOutput`, `TmpBuffer` and `TmpNextBuffer` as scratch (`TmpLightAtten`, `TmpTangent`, `TmpLightAcc`). The prologue's result is consumed by `AlphaTest` before lighting starts, and the body zeroes `TmpOutput` again before its stages, as it does today.

Why the kill decision is unchanged: the alpha lane of each operation depends only on the alpha lanes of its operands under the eligibility rules, and the prologue runs the same operation, clamp, rounding and scale per stage. An identity stage only re-clamps a value already in [0, 1].

### 6.4 IR and encoder points to check (`usse_ir.cpp`)

- `HoistLoadsGlobal` ends its entry block at the first `PhaseStart`. With the kill early, hoisted uniform-memory loads land before the kill. That is correct but lengthens the part before the kill for programs with lights loaded from memory; look at the listing of one such program.
- `HoistLoads` forgets known loads at `PhaseStart`; no change needed.
- Do not add or enable any pass that folds quad moves into partial-mask four-wide ops or folds loads into second sources. The prologue uses only four-wide forms through existing `LoadArg` and `ColorOp` paths.
- Keep one kill and two parts. Three parts and a second kill both hung the console's GPU on 2026-09-05.

### 6.5 Rasterizer changes (`gxm_rasterizer.cpp`)

- At initialisation, next to `Usse::SetDebugUnlit`: `Usse::SetEarlyKill(!(GxmFlag("noearlykill") || file ux0:data/mh4u/noearlykill exists))`, plus the check and widen flags. Set once before any emission; the tier-2 worker reads the same values.
- In `BindPrograms`, add to the `fs_hash` combine: 0x200 when early kill is off, 0x400 for the check mode, 0x800 for `earlykillall`. This keeps the in-memory and disk caches apart.
- The emitter source change alters `GXM_PROGRAM_VERSION`, so `programs.bin` starts over. Measure on the second run of a scene (warm cache).

### 6.6 Switches and checks

- **Kill switches:** `noearlykill` (diag) and `ux0:data/mh4u/noearlykill` (release), read once like `nopreshade`.
- **`earlykillcheck` (diag):** emit the prologue, copy its alpha to a spare quad (virtual 120-123, after `TmpSnap`), keep the kill at the end as today, and before the pack overwrite the output with magenta where the two alphas differ. Use a test into P2 and predicated moves, the forms the border-colour replacement uses. Any magenta pixel is a mismatch; expect none.
- **Host check:** dump the top area-3 programs with `usse_dump`, rebuild `tools/usse_host/fs_emit`, and read the `USSE_IR_DUMP=1` listing of two or three: one kill, two phase starts, the prologue before the kill.
- **Stats:** add to the census line how many alpha-tested draws got the early form.

### 6.7 Verification

1. Diag `timedraw scenetime`, slot 2, still camera: main target GPU time against 33.3 ms. Target 28 ms or less.
2. `earlykillcheck` through slot 2 with the camera script, then deck, ship cutscene, village walk, character select, title screen (its alpha-tested sprites are the programs that hung before).
3. Release `cam.sh`: shown fps per segment, `gpuwait`, and the skip counters. Target: 30 in every segment except possibly "left".
4. Screenshots with and without `noearlykill` at the same still frame; they should be identical apart from animation.
5. Deck GPU time unchanged (alpha was +0.3 ms there).

### 6.8 Risks

- **A form the console computes differently.** Mitigated by reusing existing code paths only, the check mode, and the kill switch file.
- **The saving is smaller than estimated** if the part after the kill still runs for rejected or hidden fragments. Phase 0 run 1 shows this before the code is written.
- **First-visit hitch** returns once, because the disk cache restarts.
- **Heap:** none; programs grow by a few dozen instructions.

## 7. Phase 2: render thread

Do this after Phase 1 is measured; with the GPU under budget the frame-end wait (~5 ms) goes away by itself.

1. **Menu plan 5.1 (F0)** as written.
2. **Menu plan 5.2-5.4 (A)** as written. In area 3 at most 27.5 of the 53 dumped HUD setups per frame differ only in texture 0 (the dump does not show every register, so the menu plan's Phase 0 counters still decide). The rest alternate two TEV configurations.
3. **Uniform uploads** (11,600 per 3 s at 21.6 us):
   - the vertex path copies the whole 132-row bank (2,112 bytes) into the reserved buffer on every dirty draw; declare and copy only the rows a program can reach (needs the vertex emitter to report its highest row and whether it indexes through an address register);
   - replace the per-uniform `SetUniformF` calls of the fragment path with a per-program list of (offset, source, count) built once in `UniformsOf`.
   - Kill switch `nouniformtrim`; check mode compares the trimmed buffer with a full write.
4. **A2** only if A's counters show more than ~20 non-texture misses per frame left in the HUD.

## 8. Relation to the menu plan

- **Pull in:** its Phase 0 counters, F0 and A. One implementation serves both.
- **Leave there:** GX trims (G) and the hidden-world skip (B).
- **Constraint K respects:** eligibility depends on `FSConfig` only, so the menu plan's finding 4 (the fragment program does not depend on which texture is bound) still holds.
- **Constraint for R and O:** they make the program choice depend on texture formats or contents. If R lands, A must treat 0x8E/0x96/0x9E as non-binding whenever the last full setup used a format-based verdict (one more bit next to `repeat_plain`). O should reuse the opacity verdict of menu plan 6.3, computed where the texture is decoded for upload rather than on the draw path.

## 9. Phase 3 candidates, by what Phase 0 and 1 show

- **R, static alpha resolve.** In `BindPrograms`: if the alpha chain reads only constants and textures without an alpha channel, through Replace and Modulate only, compute the alpha from the TEV constants. If it passes the test with a margin of at least 0.01 of a step, use the Always program; if it clearly fails, skip the draw; otherwise keep the kill. Switches `noalpharesolve` and `ux0:data/mh4u/noalpharesolve`; check mode draws with the kill program and counts rejected fragments' draws as mismatches.
- **FS1.** Track present ops in their own average and estimate the backlog as `ops older than the current frame * avg_op_us + pending presents * avg_present_us`. Do it only if the release counters show backlog-only skips.
- **FS2.** After a frame boundary that applied a skip, render the next frame unless three presents are pending. Do it only if the counters show consecutive skips.
- **S.** In Single screen, drop the display transfer whose output is the bottom screen so the leading clear quad no longer forms its own scene. Check that switching the layout in the pause menu brings the screen back.

## 10. Expected trajectory (estimates)

| After | GPU per frame | Render thread | Shown fps at slot 2 |
|---|---|---|---|
| Today | 33.3 ms | ~95% in diag, ~5 ms of it GPU wait | 28-29, dips to 23-24 |
| Phase 1 | ~24-28 ms | GPU wait mostly gone | 29-30, dips to ~27 |
| Phase 2 | same | 2-3 ms per frame less in diag | 30, dips rarer |
| Phase 3 as needed | 1-3 ms less | - | 30 steady |

### Critical Files for Implementation
- src/video_core/renderer_gxm/usse/fs_usse_gen.cpp
- src/video_core/renderer_gxm/usse/fs_usse_gen.h
- src/video_core/renderer_gxm/gxm_rasterizer.cpp
- src/video_core/renderer_gxm/usse/usse_ir.cpp
- src/video_core/gpu.cpp (with gpu_thread.cpp and src/citra_vita/citra_vita_main.cpp for the Phase 0 counters)
