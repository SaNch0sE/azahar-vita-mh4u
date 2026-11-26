# Strip Runs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consecutive indexed-strip draws that differ only in their index list (registers 0x227/0x228) share the first draw's finished setup on the render thread, so the GPU gets the same draws with less render-thread CPU per draw.

**Architecture:** The parse side holds such strips as a run (like the existing geometry point run: `HeldRun`/`MergeDraw`/`FlushRun`) and sends it as one `Op::StripRun` record. The render side draws the run's first draw through `RasterizerSgxd::Draw`, which keeps its submission when it is shareable; each later draw cleans the CPU cache for its own index list and vertex range and resubmits the kept submission with its own index address and count, or falls back to the full path.

**Tech Stack:** C++20, VitaSDK, the sgxd renderer (`src/video_core/renderer_sgxd`), sgxd recipes (`src/sgxd/sgxd_recipe.h`). No host unit tests cover this renderer; the tests are console runs with tools-build counters and an equality check built into the tools build.

**Spec:** `docs/superpowers/specs/2026-11-26-strip-run-draws-design.md`

## Global Constraints

- Every game, not behind a title tuning. The tools-build sgxd flag `nostripruns` turns the run off (for the A/B test only).
- The GPU must receive the same draws in the same order: same `SgxdRecipeDraw` and `SgxdRecipeDrawPrograms` per draw as today.
- Only indexed, non-geometry, triangle-strip draws with 16-bit indices and 7 or more vertices join; at most 16 draws per run.
- One commit per task, with its measurement in the message; commit trailer lines as the branch uses (Co-Authored-By / Claude-Session).
- Nothing personal in commits (no home paths, user names, IPs); the branch's grep check before any push.
- Console tests: one run at a time, the user's settings untouched (the scripts back up and restore), Azahar quit after each run (the scripts do it; never `app close` it).
- Build: `export VITASDK=$HOME/vitasdk PATH=$HOME/vitasdk/bin:$PATH && cmake --build cmake-build-release-tools -j16` (tools build; speed numbers). Release check at the end: `cmake --build cmake-build-release -j16`.
- Never lower the render resolution.

## Review Focus

1. A strip that samples the buffer it draws into (heat haze, refraction): must take the full path for the whole run, so it never reads stale pixels. Test: Task 3's equality check plus the `stripcheck` mismatch count = 0, and Task 3's "not shareable: self read" counter is nonzero in at least one scene or the code path is reviewed by reading.
2. A pass ending mid-run (data room, draw cap, code-heap flush, PDS half flip): later draws must not reuse a submission from a closed pass. Test: Task 3's fallback counter "pass changed" and the equality check.
3. An index list that is unscanned, uncached, or outside guest memory: the later draw must take the full path or the same branch `VertexStageFor` takes. Test: Task 1's shared helper is the only code that decides it; Task 3 counts "fallback: index range".
4. A run broken by any other record (uniform upload, texture copy, fill, present): the run must be sent before that record. Test: Task 2's run/record counters; picture unchanged in the A/B runs.
5. Diagnostics/vprof draw numbering: a shared draw steps `draw_serial` and counts as a draw everywhere a draw is counted. Test: Task 4's tools cpu line shows the same draw count per second with `nostripruns` and without (±2%).

---

### Task 1: One helper decides and cleans an indexed draw's reads

No behaviour change. It moves the index-list scan and the cache cleaning out of `VertexStageFor`, so the shared path in Task 3 calls the same code.

**Files:**
- Modify: `src/video_core/renderer_sgxd/sgxd_rasterizer.h` (declarations near `struct VertexStage`, ~line 575)
- Modify: `src/video_core/renderer_sgxd/sgxd_rasterizer.cpp` (`VertexStageFor`, ~lines 7181-7415)

**Interfaces:**
- Produces:
  - `struct RasterizerSgxd::VertexFeed { u32 reg; PAddr address; u32 stride; u32 words; u32 loader; };` (the local `Feed`, moved to class scope).
  - `struct RasterizerSgxd::IndexedReads { bool mapped; bool scan; u32 indices_gpu; u32 vertex_min; u32 vertex_max; };`
  - `IndexedReads RasterizerSgxd::ReadIndexedList(std::span<const VertexFeed> feeds, u16 input_regs, PAddr index_address, bool index_u16, u32 count, Usse::VsMode mode, const Describe& describe);` holds today's code from "--- the index list ---" through the 16-bit/8-bit branches: the mapped test (returns `mapped = false` where `VertexStageFor` returns an empty stage today), the scan decision, `scanned_indices`/`FindMinMax`, and the index list's own `ReadsCached`/`CachedRead` or `SgxdCacheSkipped`.
  - `void RasterizerSgxd::CleanVertexReads(std::span<const VertexFeed> feeds, u16 input_regs, u32 first, const IndexedReads& r);` holds the per-feed cache cleaning from the fetch loop (the `ASSERT_MSG` on the mapped range, `IsUncached`/`cleaned_geometry` skip, `ReadsCached`/`CachedRead`). The fetch building stays in `VertexStageFor`.

- [ ] **Step 1: Move the code into the two helpers and call them from `VertexStageFor`.** `VertexStageFor` keeps its exact order of effects: index list first, then for each fetched feed the fetch entry, then its cleaning. If keeping the order means calling `CleanVertexReads` once after the fetch loop, check that nothing between them reads the cleaned state (nothing does today: the loop only builds `fetch`).
- [ ] **Step 2: Build.** Run the tools build command. Expected: `rc=0`, no new warnings in `build_tools.log` (`grep -c warning` same as before the change).
- [ ] **Step 3: Measure no change.** Run `SLOT=1 bash vita-tests/b1flyplay2.sh t1 $PWD/cmake-build-release-tools/src/citra_vita/citra_vita.vpk`. Expected: game fps per second within ±1 of run `merge4` (`out/b1p_1_merge4.txt`), the same "other draws" and "repeats" counts (±5%), "closed, vita responding".
- [ ] **Step 4: Commit** `sgxd: an indexed draw's index scan and cache cleaning in their own functions` with the step 3 numbers.

### Task 2: The parse holds strip runs; the render side replays each draw in full

Behaviour identical (every draw still goes through `Draw`). This proves the hold, the record and the register handling before any sharing.

**Files:**
- Modify: `src/video_core/renderer_sgxd/render_thread.h` (`HeldRun` area, ~line 600; the `Op` enum is in the .cpp)
- Modify: `src/video_core/renderer_sgxd/render_thread.cpp` (`Op` enum ~line 85; `ParsedOutput::AccelerateDrawBatch` ~line 240; `MergeDraw`/`FlushRun`/`ParsedBegin` ~lines 683-760; the render-side dispatch `case Op::Draw` ~line 4371; the per-op stats switches at ~4157 and the tools draw tests at ~2395/4014)
- Modify: `src/citra_vita/citra_vita_main.cpp` (cpu line, beside the "parse strips" text)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `Op::StripRun` record: `u32 n`, then `n` entries of `struct StripRunDraw { u32 index_reg; u32 count; DrawIndexRange indices; };` (`index_reg` = register 0x227's value, `count` = 0x228's).
  - `struct HeldStrips { u32 draws = 0; std::array<StripRunDraw, 16> list{}; u8 is_indexed{}; decltype(Pica::RegsInternal::reg_array) regs{}; };` member `held_strips`; `bool RenderThread::JoinStrip(bool is_indexed);` `bool RenderThread::StartsStrip(bool is_indexed) const;` and the existing `FlushRun()` sends a held strip run too (one held kind at a time: a strip run and a geometry run never coexist, because starting one sends the other).
  - Render side: `void RenderThread::DrawStripRun(const StripRunDraw* list, u32 n);`
  - Tools counters (`ToolsStrips`, extend the existing array): runs sent, draws in runs.

- [ ] **Step 1: Parse side.**
  - `StartsStrip`: true for `is_indexed`, `use_gs == No`, topology Strip, `index_array.format != 0`, `num_vertices >= 7`, and `!SgxdFlag("nostripruns")` (read once into a static).
  - `JoinStrip`: `held_strips.draws != 0 && held_strips.draws < 16 && StartsStrip(...)` and the same checks as `MergeDraw` with registers `PICA_REG_INDEX(pipeline.index_array)` and `PICA_REG_INDEX(pipeline.num_vertices)` as the only ones allowed to differ from `held_strips.regs`. Write it by sharing `MergeDraw`'s check (a helper `bool OnlyRegsChanged(std::initializer_list<u32> allowed, const decltype(...)& held_regs) const` that both call), not by copying it.
  - In `ParsedOutput::AccelerateDrawBatch`: after `MergeDraw`, try `JoinStrip`; on a start, `SyncParsedState`, `PrefetchFragment`, `PrefetchVertex` as today, then hold it the way the geometry run holds its first draw (entry 0 = this draw, `regs` = the state as synced).
  - `FlushRun`: a strip run of 1 goes as today's `ParsedPut(Op::Draw, indices, 1)`; of 2+ as one `Op::StripRun`. Per draw, keep the later draws' register writes marked, exactly as `MergeDraw` does (they go with the next state, after the run's record).
  - Every switch/test over ops that lists `Op::MergedDraw` (stats ~4157, tools ~2395/4014) lists `Op::StripRun` too.
- [ ] **Step 2: Render side.** `case Op::StripRun:` calls `DrawStripRun`. For each entry, in order: set registers 0x227/0x228 to its values and mark them dirty in `render_pica.dirty_regs`, `ScannedIndices` if `scanned`, `GeometryCleaned`, `AccelerateDrawBatch(true)`, `GsInvoked(render_pica)`. After the run, put 0x227/0x228 back to the first entry's values and mark them dirty, as `DrawMerged` does with its registers.
- [ ] **Step 3: Build.** Expected `rc=0`.
- [ ] **Step 4: Measure.** Run `SLOT=1 bash vita-tests/b1flyplay2.sh t2 <vpk>`. Expected:
  - the cpu line's strip-run counters show ~700 runs/s and ~2100 draws in runs (the dry run measured ~700 runs joining ~1400 draws);
  - "other draws" per second the same as `t1` (±5%): every draw still reaches `Draw`;
  - fps within ±1 of `t1`; "asleep for records" not higher than `t1` by more than 10 ms/s.
- [ ] **Step 5: Measure with the flag.** `SLOT=1 bash vita-tests/b1flyplay2.sh t2off <vpk> "nostripruns"`. Expected: runs 0.
- [ ] **Step 6: Commit** `sgxd: the parse holds indexed strip runs (each still drawn in full)` with the numbers.

### Task 3: Draw keeps a shareable submission; the tools build checks it against the full path

Still no sharing on screen. Every draw still goes through `Draw`, and the kept submission is compared with what `Draw` builds.

**Files:**
- Modify: `src/video_core/renderer_sgxd/sgxd_rasterizer.h`, `sgxd_rasterizer.cpp` (`Draw`, ~8409-9913; `AccelerateDrawBatch` ~6979)
- Modify: `src/video_core/renderer_sgxd/sgxd_backend.h/.cpp` (`SgxdBackend::Draw` ~2468)
- Modify: `src/video_core/renderer_sgxd/render_thread.cpp` (`DrawStripRun`)
- Modify: `src/citra_vita/citra_vita_main.cpp` (cpu line)

**Interfaces:**
- Consumes: Task 1's `VertexFeed`, `IndexedReads`, `ReadIndexedList`, `CleanVertexReads`; Task 2's `DrawStripRun`.
- Produces:
  - `struct RasterizerSgxd::SharedStrip { bool ok; SgxdRecipeDraw draw; SgxdRecipeDrawPrograms p; std::array<VertexFeed, 16> feeds; u32 feed_count; u16 input_regs; u32 pass_mark; u32 data_opens; u32 pds_generation; const void* fragment; };` member `shared_strip`.
  - `void RasterizerSgxd::BeginStripRun();` arms the keeping (`shared_strip = {}` and `strip_run_armed = true`); `void RasterizerSgxd::EndStripRun();` disarms and clears.
  - `enum class StripRefusal : u8 { None, NotKept, PassChanged, NoRoom, IndexRange, Count_ };`
  - `StripRefusal RasterizerSgxd::DrawSharedStrip(u32 count, PAddr index_address, std::optional<std::pair<u32, u32>> scanned, u16 cleaned, bool submit);` (Task 4 passes `submit = true`; this task passes `false`).
  - `std::pair<const SgxdRecipeDraw*, const SgxdRecipeDrawPrograms*> SgxdBackend::LastDraw() const;` (the last `Draw` call's copies; null when the pass has none).
  - `u32 SgxdBackend::PdsGeneration() const;` stepped wherever `FlipPdsHalf` clears `pds_index`/`secondary_kept`.

- [ ] **Step 1: Keep the submission.** In `Draw`, only while `strip_run_armed`, fill `shared_strip` right before `backend.Draw(draw, p)`, and set `ok` only when all of these hold:
  - not immediate, `is_indexed`, `use_gs == No`, topology Strip, 16-bit indices, `count >= 7`, `!stage.fresh`;
  - `!keep_alpha_opaque`, `draw_own_pixel == 0`;
  - neither `FitWDepth` nor `MapZOnW` ran for this draw (a local flag set where each is called);
  - this draw ended no pass for a self read, wrote no depth back and no colour back (`depth_units == 0`, `owned_units == 0`, `TexturesReadPassColour()` false);
  - no unit the fragment program samples (`Usse::SampledTextureUnits(FragmentConfig())`) reads a range overlapping the pass's colour buffer or its depth buffer. Compare the `unit_reads` ranges from `TextureReads` with the colour target's `[addr, End())` and the depth target's range.

  Also store the feeds and input regs (from `VertexStageFor`; return them through `VertexStage` or a member), `backend.PassMark()`, `backend.DataOpens()`, `backend.PdsGeneration()`, and the fragment program pointer.
- [ ] **Step 2: `DrawSharedStrip`.** Refuse with:
  - `NotKept` when `!shared_strip.ok`;
  - `PassChanged` when the pass mark, data opens or PDS generation differ from the kept ones, or `!backend.InPass()`;
  - `NoRoom` when `backend.PassDraws() + 5 > MaxDraws` (the same test `Draw` makes);
  - `IndexRange` when `ReadIndexedList` gives `!mapped`.

  Otherwise:
  - set `scanned_indices`/`cleaned_geometry` as `AccelerateDrawBatch`'s caller would;
  - call `ReadIndexedList(..., Usse::VsMode::Draw, ...)` and `CleanVertexReads(..., first = 0, ...)`;
  - build `p = shared_strip.p` with `p.indices = r.indices_gpu`, and `draw = shared_strip.draw` with `draw.count = count`;
  - when `submit`: call `backend.Draw(draw, p)`, then do the per-draw bookkeeping `Draw` does: `draw_serial++` (plus `VprofDraw`/`ReplaySkipsDraw` under `VITA_DIAGNOSTICS`), `pass_guest_draws++`, `pass_drawn_lo/hi` widening from `DrawnRows()`, the kept fragment program's `draws++`, the tools tallies (`tools_sections`, `ToolsMergeNote`, `tools_tally`);
  - always: reset `scanned_indices`, `cleaned_geometry` and `gs_indices_set` as `AccelerateDrawBatch` does.
- [ ] **Step 3: The check (tools build, flag `stripcheck`).** In `DrawStripRun`, call `BeginStripRun()` before the first entry and `EndStripRun()` after the last. With `stripcheck`, for entries 2..n:
  - first call `DrawSharedStrip(..., submit = false)` and keep the `draw`/`p` it built (return them through an out-parameter or a member `last_shared`);
  - then run the entry through the full path as in Task 2;
  - then compare with `backend.LastDraw()`. Every field of `SgxdRecipeDrawPrograms` must be equal (memcmp is valid: add `static_assert(std::has_unique_object_representations_v<SgxdRecipeDrawPrograms>)`; if the floats in `viewport` defeat that assert, compare field by field with the floats bitwise). All fields of `SgxdRecipeDraw` but `programs` must be equal.
  - Count checked draws, mismatches by field (first differing field name), and refusals by `StripRefusal`. `LOG_ERROR` the first 8 mismatches with both values.
- [ ] **Step 4: Build.** Expected `rc=0`.
- [ ] **Step 5: Run the check on MHXX.** `SLOT=1 bash vita-tests/b1flyplay2.sh t3x <vpk> "stripcheck"`. Expected:
  - checked draws ~1400/s;
  - mismatches **0**;
  - refusals listed, mostly `None`.

  Any mismatch: stop, find which state differs, and add it to the step 1 conditions or the step 2 refusals. Never paper over it in the compare.
- [ ] **Step 6: Run the check on MH4U.** `SLOT=6 bash vita-tests/b1flyplay4.sh t3m <vpk> "stripcheck"` (slot 6: the Rathian area with heavy effects). Expected: mismatches **0**. Record checked draws/s.
- [ ] **Step 7: Commit** `sgxd: a strip run's first draw keeps its submission; tools builds check it against the full path` with both runs' numbers.

### Task 4: Share the submission

**Files:**
- Modify: `src/video_core/renderer_sgxd/render_thread.cpp` (`DrawStripRun`)
- Modify: `src/citra_vita/citra_vita_main.cpp` (cpu line: shared draws, refusals)

**Interfaces:**
- Consumes: Task 3's `BeginStripRun`, `EndStripRun`, `DrawSharedStrip(..., submit)`, `StripRefusal`.

- [ ] **Step 1: Share.** Without `stripcheck`, for entries 2..n:
  - set 0x227/0x228 as in Task 2 (the registers stay as the guest had them);
  - call `DrawSharedStrip(..., submit = true)`;
  - on any refusal, run the entry through the full path as in Task 2.

  Every later entry tries sharing again, because a full-path draw while armed refreshes `shared_strip`.
- [ ] **Step 2: Build.** Expected `rc=0`.
- [ ] **Step 3: Check once more on MHXX with `stripcheck`** (Task 3 step 5). Expected: mismatches 0. This confirms the check still runs and step 1 didn't move state.
- [ ] **Step 4: A/B on MHXX slot 1.** Run alternately, `on1` (no flag), `off1` (`nostripruns`), `on2`, `off2`, with `SLOT=1 bash vita-tests/b1flyplay2.sh <tag> <vpk> [flag]`. Expected, the medians over seconds 2-22:
  - render thread "running records" lower with the run on by at least 30 ms/s (the design expects ~50);
  - shared draws ~1400/s;
  - draws per second equal within ±2%;
  - fps per second not lower than off at any second by more than 1;
  - "asleep for records" not higher by more than 10 ms/s.
- [ ] **Step 5: A/B on MH4U slot 6.** The same with `b1flyplay4.sh`, `SLOT=6`. Expected: the render thread lower or equal, and fps not lower.
- [ ] **Step 6: Picture check.** Take screenshots on and off at the same second (`SHOT=1` env with `b1pace.sh`, as the scripts support) in MHXX slot 1 and MH4U slot 6. Expected: no visible difference. Small timing differences in moving effects are acceptable; missing or wrong geometry is not.
- [ ] **Step 7: Commit** `sgxd: a strip run's later draws share its first draw's submission` with the A/B numbers.

### Task 5: Release build and hand-off

**Files:** none changed unless the checks find something.

- [ ] **Step 1: Build release.** `cmake --build cmake-build-release -j16`. Expected `rc=0`.
- [ ] **Step 2: Privacy check.** Run the branch's usual personal-data grep (home paths, user and
  machine names, local IPs) over `git log -6 --format=%B` and `git diff 096f072c5..`, and
  `strings` over the release eboot. Expected: no matches.
- [ ] **Step 3: Install the release build for the user's play test** (the same way the 096f072c5 build was installed). Report the A/B numbers and what to watch for: any missing, flickering or wrong-coloured models.
