# Monster Hunter 4 Ultimate on the PS Vita: performance notes

Measurements on a PCH-1108 (HENkaku Ensō 3.65, CPU at 500 MHz, GPU at 222 MHz) with the US release and
the v1.3 update, at the ship's deck right after the save loads, no frame-cap cheat unless stated.
"Speed" is Azahar's emulation speed (100% = 60 guest fps); "shown" is frames presented per second.

## Where it stands

| Step | Deck speed | Shown fps |
| --- | --- | --- |
| Start of this work (colours fixed) | 24% | 15 |
| SVC clock reads, wake-ups, glyph state repeats | 31% | 18-19 |
| Short-vector fallback (page table, chunks, host VFP) | 33-38% | 20-23 |
| Kernel plugin v2: GetThreadId/GetSystemTick on core | 51-57% | ~20 |
| Glyph memo, texture re-upload hash | 55-57% | ~20 |
| TEV rounding at the last stage only | 45-65% | 21-22 |
| Procedural texture baked into a strip (GPU 48.8 -> 38.6 ms) | 55-60% | 25-26 |
| Fog strip, light pre-normalise, shadow fold, Fresnel skip (-> 36.9 ms) | 55-60% | 26-27 |
| No post fx / shadow-target clear, colour-only scene merge (-> 35.5 ms) | 60% | 28-29 |
| Reflection LUTs as 1 (-> 32.9 ms of GPU a frame) | 60-63% | **30-31** |
| Glyph memo on kept fingerprints, hardware draws keep fragment setup | 64-68% | 21-22 |
| 30 fps cap cheat on; repeated presents not queued (deck now measured in real frames) | 90-93% | 27-28 |
| Per-vertex normal and N.V (deck GPU ~32.4 -> ~30.4 ms) | 90-93% | 27-28 |
| Single-screen layout (Heavenfall mod), bottom screen not drawn | 90-93% | 27-28 |
| DSP mixing skips silent sends; vertex cache (copy 24 -> 3.8 MB/s) | **100%** | **30** |

From the 30 fps cap row on, speed is against the capped 30 game fps (100% = full speed at 30 fps)
and the shown fps counts distinct pictures. The ship's deck now runs at full speed; the next limits
are the emulation thread's command-list processing and, close behind, the GPU (~28-30 ms a frame).

## Emulation thread (core 0 + the guest on core 2)

- **System calls.** ~11,000/s on the deck, 93% of them `svcGetThreadId` (half for the current
  thread, half for one other handle) and `svcGetSystemTick`. Plugin v2 answers both in the svc
  vector stub (`azahar_fast_svc`, `vita_native/src/native.c`); exits fell from ~13,500/s to ~2,000/s.
  Kill switch: `ux0:data/azahar/nofastsvc`.
- **Clock reads.** `std::chrono::high_resolution_clock::now()` costs ~10 us on the Vita. PerfStats
  read it twice per svc (now skipped on Vita). Use `Common::PipelineStats::NowUs()`
  (`sceKernelGetProcessTimeWide`) for timing on the console.
- **VFP short vectors.** One 1525-instruction matrix routine (ending at guest PC `0x00110DA8`, FPSCR
  LEN=3) runs ~60 times/s in the DynCom fallback. It cost 3.5 ms a block; now ~0.9 ms. The interpreter's
  `SetPageTable` emptied its translation cache every block; it runs in 128-instruction chunks
  (`RunUntilSvc` stops before an svc); single-precision VFP ops use host floats on the Vita (checked
  against soft-float: 0 mismatches in 20,000).
- **GSP command lists.** `TriggerCmdReqQueue` is ~2.4 ms per call; per draw ~34 us on the emulation
  thread (vertex copy into the uncached ring ~13 us, `ShipDraw` ~10 us). A cached ring is not an
  option: the game partition has no room for it.
- **Timing events** are ~4% (mostly HLE DSP ticks).

## Render thread

- **Geometry-shader draws** (the text: one point per glyph, expanded by the GS) run their shaders in
  software. JIT (ARM32 shader JIT on a VM block) plus a draw memo (98% of glyph draws repeat) took
  this from 48% to 27% of the render thread. Kill switches: `shader_interpreter`, `nogsmemo`.
- **Repeated software draws** with the same fragment-side registers skip state sync (83% of them).
  Switch: `norepeat` in `gxm_flags.txt`.
- **Textures** were re-decoded ~70 times/s: partial invalidations (page-granular write tracking of
  textures that share pages with other data) of bytes that had not changed. The whole-texture hash
  check took the texture phase from 29% to 10%.
- **The GPU profiler build costs CPU time** (~44 us per program switch). Build without
  `ENABLE_VITA_GPUPROF` for anything but GPU captures.

## GPU (psp2-cex-gpues4-prof captures)

- Fragment-ALU bound, almost no texture stall. One lit world program (`us_00000025bbe5f060`: one
  light, shadow, LUT 6 + LUT 3, fog) is ~40% of fragment time; the top four lit programs ~72%.
- Its cost by elimination: lighting and sampling ~60%, TEV stages 1-5 ~29%, fog ~11%.
- Rounding to 8 bits only after the last active TEV stage: -10% fragment time per draw, visually
  identical (`tevround` restores per-stage rounding). Loading Replace arguments into the result
  directly: a little more (`noreplacedirect`).
- Not worth it / ruled out: hardware-filtered lighting LUTs (F32 LUTs cannot be filtered, and the
  PICA's per-entry deltas and signed wrap do not map onto it); depth load/store per scene (turning
  both off changed nothing).

## Miscompiles found on the console

The A9 + SGX543 compute some instruction forms differently from what the IR means:

- Four-wide vector ops reading a temporary as the second source after `LoadSources` folded a load
  (vertex programs): the whole scene lost its blue channel. Only secondary-attribute sources are
  folded now.
- Folding quad moves into partial-mask four-wide ops reading secondary attributes (fragment programs):
  the same olive cast. Not done.
- `vrintm` (ARMv8) in the shader JIT faults on the A9; floor is truncate-and-adjust on the Vita.

Tools: `usse_stopN`, `usse_vsnoloadsrc`/`usse_fsnoloadsrc` bisect tier-2 passes on the console;
`tools/usse_host` replays dumped programs on the PC (`usse_dump` writes them).

## Render thread breakdown (after all of the above)

Per 3 s on the deck: the render thread is ~91% busy.

| Part | Time | Per op |
| --- | --- | --- |
| Hardware draws (~6,000) | ~815 ms | ~135 us |
| Software draws (~12,900, mostly memoized glyphs) | ~940 ms | ~73 us |
| Applying shipped state (~590,000 register writes) | ~205 ms | ~11 us |
| Waiting for the GPU at frame end | ~620 ms | - |

A hardware draw is ~7 us of vertex layout, ~9 us of vertex program lookup and ~118 us of the GXM
phases every fully set-up draw pays (programs ~30, textures ~28, framebuffer ~23, draw ~23,
uniforms ~17, scene ~12, invalidate ~11 us).

Since then:

- ~43% of hardware draws keep the previous draw's fragment-side registers with nothing in between.
  These now skip the framebuffer, texture and scene phases and rebind only the programs and
  uniforms (switch: `nobeginrepeat`). Per 3 s, the framebuffer phase fell from ~194 ms to ~134 ms
  and textures from ~230 ms to ~198 ms.
- The glyph memo is keyed on uniform fingerprints that are kept up to date as uniforms are written,
  instead of hashing ~1.5 KB per shader per draw.

## Open (in the order being worked on)

1. **Applying shipped state on the render thread** (measured; mostly done). Per 3 s on the deck:
   ~19,000 shipments, ~340 ms with timers in place (one `sceKernelGetProcessTimeWide` read is
   ~0.7 us). ~2,800 of them carry shader code: the game switches between different programs,
   not identical re-uploads. The mirror compared ~400 words per switch and later rehashed them;
   now the emulation thread ships the hashes with the code, the mirror copies without comparing
   (or not at all if it already holds that program), and the code buffers are reused: 73 ms ->
   24 ms. What is left (~7% of the render thread: ~31 scattered register writes, ~17 float
   registers compared per shipment) is mostly cache misses on data written by the other core.
2. **Lighting cost on the GPU** (looked at; parked). Fewer instructions do cut fragment time,
   but what is left is structural: of the top program's 214 instructions, 70 are moves between
   the three internal registers and temporaries (stores and reloads around every 4-wide value).
   Only 2 can be removed without touching the forms that miscompile on the console; more needs
   a new register allocator. Each LUT read waits for its sample at once, but the profile showed
   almost no texture stall, so overlapping the two reads is not expected to pay.
3. **Ship cutscene** (fixed). Two separate bugs:
   - Depth of field blurred every pixel: MH4U builds the focus mask with depth-tested
     full-screen quads that colour-grade the frame from the target itself (a feedback read) and
     write a constant alpha. Feedback draws were dropped, which left alpha 1 everywhere. A
     feedback draw whose TEV alpha does not depend on the self-read unit is now drawn writing
     alpha only (`nofeedbackalpha` turns it off); the colour grade on those pixels is lost.
   - Blocks of noise over lit surfaces (the shot looking up at the captain, the close-ups):
     lights whose parameters do not fit the uniform registers are loaded from memory in the
     fragment program, and one 16-float load filled four register quads. The USSE IR models a
     load as defining one quad, so the register allocator placed the other three where nothing
     wrote, and the light colours and distance attenuation came from leftover register contents.
     Each quad now has its own load. Found by stopping the fragment programs after each TEV
     stage (`dbgstage<N>`), then the debug outputs, then the host emitter's allocated listing.
4. **Shown fps (~20-22): the GPU's fragment programs** (2026-11-13). `scenetime` (with
   `timedraw`) waits for each scene and logs GPU time per target, split into the vertex phase
   (vertex notification) and the rest. On the ship's deck the main 256x512 target takes ~48 ms
   of GPU per frame (4 scenes, ~125 draws): vertex ~4 ms, fragment ~44 ms. With every program
   cut to vertex colour (`dbgminfs`) it is ~11 ms. Putting features back one at a time
   (`minkeep_tex`, `_tev`, `_light`, `_proctex`, `_alpha`), per frame: textures +5, the real
   combiner +4, lighting +11, the procedural texture +12, fog +2, alpha test +0.3.
   (`dbgout_*` only replace the output; the whole program still runs, so they do not price
   anything.) A shown 30 fps needs the frame under ~33 ms: ~15 ms of GPU to find.
   - The procedural texture (18 configurations: no noise, no shift, combiner U, linear,
     clamp to edge) is a function of one clamped coordinate. It is now baked on the CPU into a
     256-texel strip (`SyncProcTexStrip`, cached by table version and settings, unit 6) that
     the program samples once: 97 -> 42 instructions, four dependent fetches -> one. Deck GPU
     time 48.8 -> 38.6 ms a frame; shown fps 20-21 -> 25-26. `noptstrip` turns it off.
   - Fog: baked into a strip like the procedural texture (`nofogstrip`). Directional lights
     without distance attenuation are normalised on the CPU (`nolightprenorm`). A white
     dummy shadow (Shadow2D on unit 0, shadow maps dropped) is folded to 1 (`noshadowfold`).
     The Fresnel LUT is skipped when no used TEV operand reads a lighting colour's alpha
     (`nofresnelskip`): MH4U's main lit program 223 -> 189 instructions, five fetches -> three.
     Deck GPU 38.6 -> ~36.9 ms a frame: fewer instructions in the main lit programs barely
     move it, so what remains is not their instruction count.
   - Lighting: see item 2.
   Render thread: ~28 ms of work a frame on the deck; ~250 geometry-shader glyph draws a
   frame on character select are 56-59% of it (memo key ~19 us, triangle copy ~9 us, repeat
   path ~2 us merged into runs; the ~18% that change state pay a full setup, ~150 us).
5. Low priority: persist tier-2 USSE programs to disk (creating them is already microseconds);
   the render thread's unaccounted ~24%.

## Emulation thread (2026-11-13, single screen, 30 fps cap)

With the bottom screen skipped, the deck runs at 90-93% speed, 27-28 fps, and the emulation thread
is the limit: ~100% busy (the frame limiter never sleeps, `limiter 0 us` on the speed line), the
render thread ~35-45%, the GPU ~28-30 ms a frame. `emuprof` per second on the deck:

| What | Time | Count |
| --- | --- | --- |
| GSP TriggerCmdReqQueue (command lists) | ~310 ms | ~150 |
| - hardware draws | ~175 ms | ~2,350 (~73 us each) |
| - of which the vertex copy out of guest memory | ~79 ms | ~25 MB |
| - glyph (geometry-shader) draws | ~45 ms | ~5,200 |
| HLE DSP tick | ~66 ms (was ~88) | ~170 |

The vertex copy runs at the CPU's ceiling for uncached memory: `copybench` measured 300-317 MB/s
with memcpy into the ring, 118 MB/s with 64-byte NEON blocks, 264 MB/s into CDRAM. Moving the copies
to a worker on core 0 did not help (the same bandwidth, and the other draws got slower), a
cached ring (`alloc_mapped_cached`, with or without `sceKernelSyncVMDomain` on each range) ends the
title ~48 s in ("No more processes"). What did work is not copying: `pica/vertex_cache.h` keeps
the GPU copy of vertex data that has not changed (guest write tracking, plus Memory::host_write_hook
and the rasterizer's invalidations for the emulator's own writes): 77% of hardware draws hit on the
deck, the copy fell to 3.8 MB/s and the deck reached 100% speed.

## Movie, village hub (2026-11-14)

The intro movie ran at 33% (10 fps). Its decoder thread polled an empty queue in a tight loop
(0x968764), so guest time never idled and every millisecond of host work (y2r, HLE) cut the
speed; the branch now sleeps 1 ms when taken (`nofmvsleep`). y2r converts on a core-0 worker,
asynchronously, finishing ~4 ms of guest time later with the emulation thread taking the rest
if the guest gets there first (`noy2rasync`). The pad and touch peeks ran on every RunLoop
return (~5000/s, ~30 us each) and now run every 2 ms. Release: intro 30 fps.

The village hub (the user's second save) was render-thread bound: game ~27 fps, 16-18 shown.
~1800 point-mode geometry-shader particle draws a second (one program, all distinct) and
~4000 full draw setups a second (textures and TEV constants change on most draws):

- GS memo skipped for programs that never hit (`nogsmemoskip`).
- Target lookup reused when nothing it depends on changed (`nofbreuse`; RasterizerCache
  MutationGen counts the cache changes that matter).
- Texture lookups memoized per configuration while the surface's modification_tick and the
  set of registered surfaces are unchanged (`notexmemo`).
- BindPipeline answers a repeated request without hashing.
- Pre-shading: a core-0 worker with its own mirror runs VS+GS for point-mode GS draws ahead of
  the render thread (`nopreshade` or ux0:data/mh4u/nopreshade).

Release hub: shown 24-25 fps, equal to the game's rate; the emulation thread (PICA command
processing, ~60% of it in the hub) is the limit again. Deck: 30 fps in both views.

## GX queue (2026-11-14, later)

With the render thread relieved, the hub was emulation-thread bound: command lists (~140 KB,
two a frame; ~20 ms a frame in diag) ran on the emulation thread after the guest's frame,
serially. Stage 1 async GX (`gxasync gxearlyp3d`) gained nothing: the idle guest waited for
the list ("gxwait: guest idle"), then the next command, the buffer swap and the vblank all
drained it again. Stage 2 (`gx_fifo`, on by default; ux0:data/mh4u/nogxfifo turns it off)
makes the GX thread an ordered queue the emulation thread alone fills - lists (copied at
submission), fills, transfers, guest-flush invalidations, and the vblank's GPU half - while
everything the guest sees stays on time (early P3D, fill/transfer completions on modelled
delays, PDC). Overlap is bounded to a frame. Placement matters: the GX thread runs on core 0
just below the DSP worker (equal priorities do not preempt; DSP ticks waited out whole lists),
the pre-shade worker on core 3; the depth frameskip signal is off under the queue (the vblank
half sees the frame's whole op stream queued).

Release hub: game 24 -> 29-31 fps (speed ~80% -> ~99%), shown 24 -> 25-28. The render thread
is the limit now (and the keyboard screen, ~500 glyph draws a frame, render-bound at ~20).
Checked: intro movie, ship cutscene, deck both views, hub, keyboard screen.

## Village hub render thread (2026-11-14, evening)

Release, standing in the hub: shown 28.2 -> 29.8 fps (the 30 fps cap), game 30. Deck and the ship
cutscene stay at 30 and look right. Per change, hub, 3 s windows:

- **Shared shader JIT cache** (`shader_jit.cpp`). Every engine compiled its own copy of each guest
  shader into the one 4 MB A32 arena, which filled in the hub ("code arena full; interpreting").
  One process-wide cache: no arena-full warning.
- **No statistics clock reads in release** (`PipelineStats::StatUs`). ~280k process-clock reads per
  3 s fed counters release never prints; the GPU thread now reads the clock per present and times
  one op in eight for the backlog average. Shown 28.2 -> 29.2.
- **Vertex layout memo** (`AccelerateShippedDraw`, `vertex_regs_version`). The mirror's version is
  bumped only by registers the layout and program key read (formats, loader configs, VS config
  words, VS output map, lighting disable, VS code/bools/ints - not buffer addresses or float
  uniform ports, which change every draw). ~12k of ~15k hardware draws hit; layout block
  199 -> 91 ms. `vsmemocheck` compares against a rebuild (0 mismatches), `novsmemo` disables.
- **Per-page guest-flush hashes** (`SurfaceBase::SetUploadHash`, `ValidateSurface`). MH4U's writes
  next to its textures made every frame re-hash whole textures: 33 MB / 215 ms per 3 s for 1.8 MB
  actually invalidated, never a change. Whole uploads keep a hash per 4 KiB page (valid only while
  the whole hash is current); the check hashes only the pages the invalid regions touch. Re-hash
  215 -> 17 ms, texture lookup misses 242 -> 46 ms, frames rendered 75 -> 86 per 3 s (diag).
  `pagehashcheck` compares against the whole hash (0 disagreements), `nopagehash` disables.

Profile reading notes (Fable's review): "programs" includes "uniforms" (the uniforms timer is inside
BindPrograms); "software vertex shading" mostly re-counts the pre-shaded draws' DrawTriangles; the
~26% "none of the above" is op time outside `times.total` (ApplyShipment etc.).

Not done yet, by expected value: a TEV-constant-colour-only begin-repeat (0xC3/0xD3 change in
~4k draws per 3 s; measure first, cf. the failed light repeat), uniform upload trimming
(316 ms / 14k uploads: write plans per program, vertex bank extent), and the per-shipment
ApplyShipment cost.

### Walking the village (2026-11-14, night)

`walk.sh` walks a fixed route of six stick segments through the village (the camera there only
follows the character). Release: shown ~29.5 fps on average; one-second dips to 26-28 when a new
direction starts, which are vblank frame skips from arena backpressure.

- **Out of heap while walking** (std::bad_alloc in release). The shipment pool grows to every
  shipment ever in flight (~950 with the GX queue) and each kept its 24 KiB lighting LUT block
  and vertex program copy for reuse: ~23 MB that never came back. Freed on release now; diag
  heap at the end of the walk 237 -> 209 MB of 240. Found with the alloc tracer's new live-by-
  call-site table (`-DVITA_ALLOC_TRACE=ON`). Heap still creeps up slowly with each new shader.
- **Third arena slot**: seconds under 28 fps 5/7 -> 3/2 per walk. (Tried before the memory fix
  it ran the process out of heap.)
- A spurious resume event mid-load opened the pause menu; it now needs a real wall-clock stop.
- **Long walks (3x the route, ~3.5 min)** still ran out of heap after the LUT fix: the pre-shade
  worker's recorded triangles (~20 KiB per geometry-shader draw) stayed as capacity in the ~1600
  pooled shipments (32 MB), and with fragmentation the heap failed at ~217 MB live of 240. Vertex
  and ring-range buffers over 64 entries are now freed on release: live heap peaks ~195 MB and
  falls back; release long walk 204 s, no crash, 29.65 fps shown; short-walk fps unchanged.
  Found with the tracer's live table, now appended to `ux0:data/mh4u/live.txt` (the TTY drops
  lines under load).

- **"Direction-change dips" were mostly the test's own screenshots.** walk.sh took a capture at
  each segment end and the capture runs on the console: the dip landed in the second second of
  nearly every segment. `NOSHOT=1 walk.sh` measures without captures; then laps over ground
  already seen hold 29-30, and the one real dip is the first steps out of the spawn (2 s at
  21-25 game fps, every session). That is first-time work: ~30 vertex/fragment programs emitted
  on the render thread in 0.7 s (1-10 ms each, plus two ~20 ms PICA JIT compiles on the GX
  thread); the render thread fell behind and the GX queue's brake (85-317 ms a second, seen with
  the new `emuprofgx` switch, which points EmuProf at the GX thread) held the emulation side.
  - **Program disk cache** (`ux0:data/mh4u/programs.bin`): every emitted program, both tiers,
    is written by a background thread and read back by key in later runs; the file is
    versioned by a build-time MD5 of the emitter sources (plus the diag flags), so a code
    change starts it over. Release, warm cache: the first steps hold 30/30 (cold: 21-23 as
    before; only the first visit ever pays). `nodiskprograms` (diag) turns it off.
  - The **second shader tier ran on core 0 at high priority**, above the GX thread, despite its
    comment (it used the ShaderCompiler role); it now has a Background role, core 3 at
    USER_LOW.
  - timedraw names slow emits/pipeline builds (`slow vs emit ...`).
- Still open: a rare single-second stall to ~13 fps (twice in all the walks so far), cause unknown.

### Save states mid-game (2026-11-14)

Pause menu (Select+Start) -> Save states. Saving and loading now work while the game runs:
- **Save crashed** after "archives": the save's `ClearAll` emptied the surface page table but
  left every surface flagged Registered; the first surface trim after the save picked one and
  hit UnregisterSurface's page assert on the render thread. ClearAll now unregisters them.
- **Load was refused** with the render thread running. Enabled: the ScopedRenderContext guard
  held a reference to the GPU the load destroys (now re-fetched), the frontend's GPU settings
  are re-applied through `System::SetGpuConfigurator`, the GXM mappings of guest RAM are dropped
  before the load frees it (`System::SetBeforeShutdown`; otherwise "could not map 134217728
  bytes" every frame and a stale mapping), and the state is decompressed straight from the
  file instead of a 28 MB heap buffer (that load died in std::bad_alloc).
- **A state saved after a load sent the guest to PC 0** when loaded: the load puts the running
  threads back from their saved contexts, which are only written on a context switch. The save
  now copies each core's live registers into its running thread's context first.
- States load across builds (`SetLoadStateAnyBuild` for every build, not just the diag
  autoload): every commit is another build, and an update would otherwise orphan them.
- The pause menu opens on Resume every time.
- A state can be loaded from the title screen; there is no need to enter the game first.
- Timings on the console (diag): save ~30 s (28 MB file), load ~10 s; the screen freezes meanwhile.

## Quest counter (2026-11-14, from the user's save state)

Release: game 22-24 fps, then 24-26 with the change below. Both worker threads are near full:
the GX thread ~870-900 ms a second (about a quarter of it waiting on the render queue) and the
render thread ~95%. A frame is ~690 draws: ~110 hardware draws of the village behind the board
(drawn every frame under a fully opaque menu, ~46k vertices) and ~450-550 geometry-shader
sprites and glyphs (one point each, ~2 output triangles) that rotate through 4-5 textures, so
most of them pay a full GXM setup (~45 us in diag) instead of joining the run before.

Render thread per 3 s (diag, timedraw): software (GS) draws ~1.4 s (~40 us each), hardware
draws ~0.77 s (~98 us each), applying shipments ~0.39 s (43.6k, ~9 us each). Dropping the
hardware draws alone only took diag from ~21 to ~23.5 fps (the GX thread is then the limit).

- The pre-shade worker (core 3) ran at USER_DEFAULT + 8, below every other thread there, and
  fell behind: the render thread shaded about a third of the GS draws itself (~230 ms per 3 s).
  It now runs at USER_HIGH + 1 under the GX queue (`preshadelow` restores the old priority):
  ~35 ms. Village walk unchanged (30).

Next candidates: a texture-only-change path for software draws (skip the target, scene, state
and program-config work when only texture bindings moved), the GX thread's ~30 us shipment
per GS draw, and the hidden world behind full-screen menus (needs a way to know the cover
before the world's draws are processed).

## Quest area 3 (2026-11-15, the user's save state, slot 2)

Release, standing and turning the camera: game 30, shown mostly 28-29 (frames dropped by the
frameskip, the uneven feel), 30 looking at sky or ground, one one-second stall to ~15. The GX
thread is light (~300-480 ms/s); the render thread is near full (diag 94%, ~5 ms a frame of it
waiting for the GPU) and the GPU is at the budget by itself.

GPU per frame on the main target (diag scenetime): 32.8 ms. Building the programs back up from
vertex colour: 7.0, + textures 10.6, + combiner 14.6, + lighting 21.6, + alpha test 29.7, all 33.5.
- Alpha test off for every draw (`ptalways` experiment): 24.9 ms. Alpha-tested draws dropped
  (`ptskip`): 20.6 ms. Alpha-tested draws with a trivial alpha-only discard program (`ptmin`):
  25.4 ms. So ~33 alpha-tested lit draws a frame cost 4.3 ms without discard and 12.2 ms with it,
  and even a trivial discard program costs them ~4.8 ms: on the SGX the cost is the discard
  itself, not the program before or after it.
- An early kill (alpha chain first, then lighting; all 2,848 alpha-tested draws per 3 s were
  eligible once combiner-buffer alpha was supported) gained nothing: 32.8 -> 32.2 ms. Reverted.
- A depth pre-pass + EQUAL colour pass would pay the ~4.8 ms discard cost again in the pre-pass:
  projected ~30.7 ms, about 2 ms. Not built.
- libgxm classes the world's ADD/ONE/ZERO programs as opaque (`logfp` pass type), so passing
  that blend as FUNC_NONE changes nothing (measured: 33.2 ms).
- A save-state load left the GXM context bound to the old renderer's freed programs (crash with
  `scenetime`); fixed (commit 2cb311511).

## Clocks (2026-11-15)

The release log now prints `[mh4u] clocks at start …` and every 30 s `[mh4u] clocks arm … MHz` (scePowerGet*). SetupClocks only raises a clock, never lowers it, and `ux0:data/mh4u/noclocks` skips it.
Measured: readback says 333 at start and 444 after SetupClocks, and stays 333 with `noclocks`, although the user's PSVshellPlus profile for MH4UVITA0 sets 500/222/222/166. GX-thread time in area 3 was the same (~400 ms/s) in all three runs, so the readback does not see the plugin's clock and the CPU speed did not change. Don't treat the readback as the real clock.

## Area 3 alpha pre-pass, in place (2026-11-15)

Opt-in `prepass` (diag flag or `ux0:data/mh4u/prepass`). An alpha-tested draw with replace blending (alphablend off, or Add/One/Zero, which is MH4U's world), an ordering depth test with depth write on, no stencil, and AlphaChainClass 1 is drawn twice. First an alpha-only kill program with colour mask NONE that writes depth; then the full program without the kill, depth EQUAL, no depth write. About 1,530 draws per 3 s qualify; the other ~1,275 alpha-tested draws really blend.

GPU per frame (diag `scenetime`, area 3):

| | ms |
|---|---|
| Baseline | 33.5 |
| `prepass` | 33.5 |
| `prepass ppskipb` (depth pass only, grass invisible) | 31.8 |
| `ptoffop` (alpha test off for the 1,530 unblended draws) | 25.6 |
| `ptoffbl` (alpha test off for the 1,275 blended draws) | 33.9 |

So the ~8 ms is in the unblended alpha-tested draws, and ~6 ms of it is the discard itself happening mid-scene, whatever the program does; the in-place colour pass costs only ~1.7 ms. The in-place pre-pass cannot recover it. Only moving the punch-through draws out of the middle of the scene could, if the cost is the GPU flushing already-tagged opaque work (Fable's model). If instead the cost is the grass cards' own overdraw being shaded, moving them would not help either. Next step: an experiment that tells the two apart, before a deferred-context build.

## Texture-only fast path for software draws (2026-11-15)

Menu plan Phase 1 (F0 + A), built as planned:
- **F0** (`UploadUniforms`): tex_dims/fb_dims dirtiness only counts for a program that declares the uniform; otherwise it stays pending. `texdimsalways` restores the old behaviour.
- **A** (`RasterizerGxm::TexOnlyRepeat`, called from `DrawTriangles` before the full path): the mirror keeps `fragment_nonbinding_version`, which texture-binding register changes don't bump (dims, addresses, formats, and filter-only config changes). When the previous software draw's setup stands except for the bindings, and every unit resolves from the texture memo, it flushes the pending run, binds the new textures and draws, without BeginDraw. Kill switch `notexrepeat` (flag or file); `texrepeatcheck` (diag) compares FSConfig hashes.

Measured at the quest counter (slot 1):
- diag: 16,632 fast-path draws per 3 s, 0 mismatches. Render-thread phases per 3 s: framebuffer 169→142 ms, textures 237→119, programs 599→370, uniforms 291→205, draw 264→178. Diag fps 21–22 → 24.
- release: 24–25 fps shown either way. The GX thread is at ~880 ms/s with the render thread relieved, so the counter is now GX-bound; the GX trims (menu plan G) come next for it.

## Frameskip for gameplay (2026-11-15)

Plan: `docs/plans/gameplay-headroom.md`. Under the GX queue at ~30 fps capacity, half of area 3's skips were false: a single late vblank (two presents pending), a backlog estimate inflated by the present op's GPU wait, and fully drawn presents dropped as superseded. Now: the presents signal needs two pending for 3 vblanks or three pending, presents stay out of `avg_op_us`, and a present is superseded only with three pending. Kill switch `oldskip` (diag flag or `ux0:data/mh4u/oldskip`). The release line gains `skip pN bN aN xN sup N` (vblanks per second each signal was on, vblanks that armed a skip, superseded presents).

Release, area 3 with the camera script: shown 28.84 -> 29.42 / 29.61; seconds under 29 from 21 of 83 to 2 of 48 and 7 of 111; armed vblanks 137 -> 48 / 75, all from sustained presents.

## Present scene without the vertex wait (2026-11-15)

The present's `BeginScene` set `SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY` as well as the fragment dependency. Only its fragment stage samples the screens, and fragment processing is in scene order, so the vertex wait only held the tiler: the next frame's world could not be tiled while this frame's was still being shaded (the ~3.4 ms of vertex work a frame lost its overlap). Without it, release area 3 with the camera script: shown 30.02 in two runs, 105/110 and 108/112 seconds at 30+, none under 29, a skip armed on 1 and 5 vblanks (from 29.4-29.6 with the frameskip change alone, 28.8 before both). Screens correct in area 3 and at the quest counter. Switch `presentvwait` (diag flag or `ux0:data/mh4u/presentvwait`) puts the wait back.

## Quest area 2 (2026-11-15, the user's new save state, slot 2)

Release: game 30, shown 24-25 in the normal view (26.3 average over the camera script), 30 looking at the ground. GX ~350 ms/s. GPU-bound: main target 40.7 ms a frame (world scene 39.6 + bottom-screen clear scene 1.1).

GPU per frame (diag `timedraw scenetime`):

| Run | ms |
|---|---|
| Baseline | 40.7 |
| `nofog` | 36.7 |
| `dbgminfs minkeep_tex minkeep_tev minkeep_alpha` (no lighting, fog) | 23.3 |
| `dbgminfs minkeep_tex minkeep_tev minkeep_light` (no alpha test, fog) | 28.3 |
| `dbgminfs dbgminsel_lit` (lit programs bare) | 8.6 |
| `dbgminfs dbgminsel_pt` (procedural texture programs bare) | 40.2 |
| `ptoffop` (alpha test off, unblended) | 35.4 |
| `ptoffbl` (alpha test off, blended) | 34.3 |
| `dbgminfs` | 7.2 |

Alpha-tested draws (diag `ptcensus`, per 15,000 binds): unblended GREATER 128 cut-outs 6,632 draws / 2.6M vertices; unblended GREATER 5/0/30; blended SrcAlpha/OneMinusSrcAlpha, GREATER 0, depth write off, 3,582 draws / 816k vertices. The world target is RGBA8; ~184 dst-alpha blends per 15,000 blended binds, all into the world target after the foliage (Zero/One colour, alpha × DstAlpha: the depth-of-field mask).

**Blended alpha-0 tests dropped** (`BindPrograms`): an alpha test that rejects only alpha 0, on a draw blended Add with SourceAlpha (or Zero) / One (or OneMinusSourceAlpha), no depth write, no stencil, into an 8-bit target, is drawn with the no-kill program. Colour is exact; into RGBA8 a rejected fragment now writes alpha 0 instead of leaving it, which only the depth-of-field mask could see. Screenshots from five camera angles matched the base build. Kill switch `noblendptdrop` (flag or file). Diag: 40.7 -> 34.2 ms. Release, camera script: shown 26.3 -> 29.0, seconds under 29 from 89 of 113 to 24 of 110.

**Bottom-screen transfer dropped in Single screen** (`GPU::QueueMemoryTransfer`): a display transfer into either bottom-screen framebuffer address is not run when the layout is Single screen (not swapped), as the bottom draws already are not. The clear quad then joins the world scene: one main scene a frame instead of two. Area 2, diag: 34.2 -> 33.7 ms. Release, camera script: shown 29.0 -> 29.6, seconds under 29 from 24 of 110 to 3 of 110. Switch `nobottomskip` (flag or file; it also keeps the bottom draws).

**Fragment trims (Fable's area 2 plan, exact ones first)** (`usse/fs_usse_gen.cpp`):
- TEV stages whose colour and alpha operations are Replace, Modulate, Add, Lerp, MultiplyThenAdd or AddThenMultiply only clamp from above: their sources are all in [0, 1], so the `max(x, 0)` was dead. AddSigned, Subtract and the dot products keep both. Diag `tevclampboth` keeps both (not in the program hash: clear the disk cache when comparing).
- Fog: the baked strip holds clamped bytes replicated in all four channels, so the fetched entry is loaded as the factor directly (the move, clamp and broadcast are gone).

Area 2, diag: 33.6 -> ~31.5 ms. Screenshots match the base build at noise level (area 2 still frame, quest counter pixel-identical). Release, camera script, two runs: shown 29.97 and 30.02, 106/112 and 115/117 seconds at 30+, at most 1 second under 29.

Fable's remaining list (docs/plans/gameplay-headroom.md): fog coordinate as one two-wide MAD (new form), fetch moved into SampleTextures, pack from the internal register (new form), specular as a CPU constant for the single-light shape, diffuse kept in the internal, LUT row from the upload, alpha test as a direct compare, and a census of unblended GREATER 0/5/30 draws whose texture alpha never falls below the reference.

## Slot 1: area 2 spot and area transitions (2026-11-15, the user's new save state)

User report: fps drops with the camera at/near the slot 1 spot in area 2, and when moving between areas; a fresh state load can perform better than after moving between areas.

Measured (release unless noted):
- Fresh load, standing at the spot: 30 shown for 20 s, no skip signals. Diag GPU 29.0-31.6 ms a frame (one main scene a frame, GPU wait ~0), so ~2-4 ms margin at the spot itself.
- `trans.sh` (scratchpad; left stick Down 6 s, stays of 20 s, screenshots per segment): the first run went from area 2 into area 3. The transition dipped for ~5 s: shown 25, 23, 30, 19, 28 (game fps 23.5/23/18 in two of those seconds, so the guest itself slowed: loading, not rendering). Area 3 afterwards (with Apceros herds) held 30 for 20 s.
- The return run (stick Up 6 s) did not leave area 3; the later segments are all area 3 at 30. So the 3 -> 2 return and "area 2 after a round trip" are **not measured yet**.

Next when resumed: fix the return direction in `trans.sh` (check the minimap/screenshot after each run), then measure area 2 at the spot after one and two round trips against the fresh-load numbers; if worse, diag `timedraw scenetime` after the round trip (GPU vs render thread, shader cache/surface counts, heap) and hand the logs to Fable.
