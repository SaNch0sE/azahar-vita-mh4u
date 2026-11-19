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
- `trans.sh` (local test script; left stick Down 6 s, stays of 20 s, screenshots per segment): the first run went from area 2 into area 3. The transition dipped for ~5 s: shown 25, 23, 30, 19, 28 (game fps 23.5/23/18 in two of those seconds, so the guest itself slowed: loading, not rendering). Area 3 afterwards (with Apceros herds) held 30 for 20 s.
- The return run (stick Up 6 s) did not leave area 3; the later segments are all area 3 at 30. So the 3 -> 2 return and "area 2 after a round trip" are **not measured yet**.

Next when resumed: fix the return direction in `trans.sh` (check the minimap/screenshot after each run), then measure area 2 at the spot after one and two round trips against the fresh-load numbers; if worse, diag `timedraw scenetime` after the round trip (GPU vs render thread, shader cache/surface counts, heap) and hand the logs to Fable.

**Round trips (2026-11-15, after the reboot):** `vita-tests/trans.sh` (a local test script kept outside the repo; running Down 6 s crosses both ways). Route 2 -> 3 -> 2 -> 3 -> 2, release: every stay held 30 shown for 20 s, area 2 after two round trips included. Each crossing dips for 1-2 s to 24-26 shown with the game's own fps falling too (loading on the guest side). No slowdown builds up across transitions in this build.

## Area crossings: mod files opened per read (2026-11-15)

Release counters `fs <ms> <reads> <KiB>` (guest File::Read through a backend without cached reads, which is LayeredFS with the Heavenfall mod). Standing: 4-5 reads, 3-6 ms a second. Crossing area 2 -> 3: 59, 164, 144, 30, 296, 134, 63 ms a second, with game fps 28, 24, 24, 30, 22, 29, 29 - one second spent 296 ms on three 8 KiB reads. `LayeredFS::ReadFile` opened the replacement file for every read. With eight replacement files kept open (least recently used goes first): one second of 100-130 ms (1.3-1.6 MB of real data) at 25-27 fps, then 30. TuizaVita 0.3B moves these reads to a worker thread instead; the remaining ~0.1 s could still go that way.

## Camp pond water (2026-11-15, the user's slot 3)

The pond was an empty pit: its programs sample a cube map (texture unit 0 type cube), which the emitter refused, so the draws were skipped (`no fragment program ... type: cube`, seven programs). Cube maps are now a linear 2D atlas of the six faces stacked (+X, -X, +Y, -Y, +Z, -Z, level 0 only), filled by `CopyTextures` from the face surfaces; `FragmentEmitter::CubeCoordinates` picks the face and coordinate from (u, v, w) as the PICA/GL do.

The water then drew with per-pixel noise. Found by stopping the combiner after each stage (`dbgstage0..5`: clean to 3, noisy from 4) and Fable reading the emitter: a colour operand with the SourceAlpha modifier on PreviousBuffer read the colour snapshot's w lane, which no instruction wrote (uninitialised register: per-thread garbage), and would have been the wrong stage's alpha anyway. `LoadArg` and `PlanCombinerBuffer` now choose the buffer half by the operand's modifier, both ways. Pond clean at 30 fps; slot 1 unchanged against a mipmap-era screenshot.

## Stall recorder and asynchronous large reads (2026-11-15)

Release prints `[mh4u] stall N ms: guest +F frames, gx G ms, render ~R ms, fs S ms N reads (+A ms async), heap H KiB` when two presents are over 100 ms apart (quiet for the first 20 s). On two area crossings it showed single file reads of 97-203 ms blocking the emulation thread. Reads of 64 KiB or more through LayeredFS now run on a worker (File::Read's async path), serialised by a lock in LayeredFS::ReadFile. Stalls over 100 ms on two crossings: 11 (max 262 ms) -> 4 (max 110 ms); one remaining stall had the guest itself produce no frame for 104 ms (its own loading). Kill switch `ux0:data/mh4u/syncreads`.

## Slowdown that builds up over a session (2026-11-15)

User report: performance drops after moving across areas in a quest (and at the quest counter after returning, down to 15 fps), never from a state saved there. Reproduced with eight area crossings from slot 1 (release): from the third crossing the game itself fell from 30 to 24-26 fps and the GX thread from ~350 to ~850 ms a second. GX profile (`emuprof emuprofgx`) early vs late: vertex-cache hits per window 2,400 -> 1,000, "miss (not wanted)" 750 -> 2,250, vertex data copied 7.5 -> 14.5 MB a second; `[vtxcache]` entries 122 -> 41 with no inserts. Cause: a key invalidated twice was volatile forever, and every area load rewrites the memory of the keys it reuses. Now volatility needs two invalidations within 2 s and expires after 10 s quiet. Eight crossings: game 30, GX 318-379 ms/s throughout.

The release `[mh4u] health` line (every 300 frames) tracks surfaces, GPU pools, shaders/pipelines and the heap; over eight crossings surfaces grew 84 -> 193, pipelines 180 -> 620, heap ~201 -> ~207 MB of a 216 MB arena - none of them the slowdown, but worth watching.

## Slot 1 gathering point (2026-11-15, the user's save in an open field)

User report: slowdown when the gathering icon appears, deeper when "... obtained" shows. Release, with screenshots: the "?" icon is on screen at load; O gathers ("Herb obtained."), L opens the item bar; shown 26-27 fps in every segment, so neither adds a measurable cost. The view is GPU-bound: ~35.5 ms a frame at the point, ~32 ms after walking away (different view; more draws, cheaper pixels). Diag scenetime here: base ~36, nofog ~32.5, ptoffop ~34, ptoffbl ~35, dbgunlit ~34.5: fog is the largest single cost in open fields (~3.5 ms), which makes the non-dependent fog read (gameplay-headroom G3) the next item.

## Fog as a non-dependent read (2026-11-15)

Every vertex program writes TEXCOORD7 = (z * 255/256 + w/512, w/2, 0, w) (o27-30; output descriptor 31 registers, TEXCOORD7 four wide). Fog programs read the strip non-dependently from it, projected by w (exactly the coordinate the shader used to compute from the fragment's depth), on the COLOR0 descriptor as the SDK's compiled imgui_f does; the texel arrives in pa4-7 before the program runs and fog no longer needs the Position input. Flipped fog bakes the strip reversed. GXP details learnt from the SDK programs in src/citra_vita/shaders and Vita3K's reader: descriptor low nibble = TEXCOORD index of the query (0xF none), 0x100/0x200 = xy / xy over w, 0x800 set (on the last query only, see "Textures as non-dependent reads"), size bits 6-7 = result registers - 1, component_info 0x30 = float result; texunit nibble 1 = non-dependent; program flag 0x800; non-dependent samplers take no data-container words.

Slot 1 open field, diag scenetime: ~32.7 ms with it, ~33.7-34.3 without; release 26-27 -> 28 shown. Screenshots match. Kill switch `nofogquery` (flag or `ux0:data/mh4u/nofogquery`).

## Out of memory in long sessions; reading the slowdowns (2026-11-15)

**Crash.** A normal session near a herd ended in `std::bad_alloc`: in-use heap 212 -> 230 MiB in
~6000 frames while surfaces, shaders and pipelines stayed flat. A sampling heap profile
(`ux0:data/mh4u/heapprof`, `src/citra_vita/heap_prof.cpp`) named `triangle_record->push_back`
in the GS draw memo: its generations were capped at 400 entries but not by size, and monster
GS draws are 100-300 KiB each (~18 MiB in 90 s). Now 3 MiB per generation and no entry over
1 MiB (`gm` on the speed line). The vertex cache's volatility/unarmed maps were bounded too.

**Partition slack.** The heap is one fixed 240 MiB memblock; thread stacks and GPU blocks come
from what is left of the game partition, which is very little. Adding ~470 KiB of static tables
(.bss) froze the game ~40 s into a slot-1 run; keep large tables on the heap.

**New release diagnostics.** `[mh4u] cpu`: CPU ms each named thread ran, `wait s/b/r/a`
(GX thread blocked: pre-shade ring, render-queue brake, vertex ring, arenas), `ps u/s/w/l`
(pre-shade used/stolen/waited, local ms), `memo hits/misses`. `gx` on the speed line is wall
time, so read it with these. `ux0:data/mh4u/gxprof` turns on the GX-thread EmuProf.

**Menus that open by themselves** (Hunter Search, Gathering Hall notice) spoil long scripted
runs: there the render thread shades ~6000 stolen GS sprite draws a second (CPU ~950 ms/s), the
worker is saturated on the rest, and the GX thread waits ~500 ms/s on the queue brake: 21-22 fps.
Ships per frame ~600 vs ~190 in play. Always check the end screenshot.

**Monster slowdown in play** (user session): same draws (~450) and ships (~190) per frame as the
fast save state, but GX wall 650-900 ms/s. Not reproduced by scripts. Next: a play session with
the `cpu`/`wait` line to tell waiting from work. Fable's plan (cheaper GS misses, memo keys on the
uniform rows a program reads, no yield-spin, AddTriangles span, HLE of the three quad programs)
is gated on that. `hwgs` in the calm part of slot 1: GX 330 vs 350 ms, render 540 vs 595 ms,
fps unchanged.

## The real cause of "slower after playing a while" (2026-11-15, fixed)

Release timers (`ux0:data/mh4u/timedraw`: `rtime`, `rt ops`, `psw` lines) narrowed it down:
near an active herd after normal play the render thread was saturated (~950 ms/s), ~650 ms of
it in software GS draws; with duplicate shading of draws the worker was already running removed
(it now waits; `preshadesteal` restores the old behaviour) the pre-shade worker was the limit,
spending ~30 us a vertex in the VS and ~42 us in the GS, against ~10 and ~18 from a save state
of the same spot. The shader JIT (A32) compiled all 4096 program words for every program
(hundreds of KiB), never frees its 4 MiB code arena, and interprets every shader compiled after
it fills; a long session filled it, a fresh boot and state load did not. Now it compiles only
the span the entry point reaches (keyed by entry point): 2-15 KiB a program, ~616 KiB of the
arena after a long session with new areas, `interp 0`, and the herd scene after play at
30/29-30 fps (GX ~340 ms, no queue waits). One GS program (entry 88, conditional jumps) still
compiles ~307 KiB: the reachability is deliberately conservative around jumps.

Every compile is logged (`[mh4u] shader JIT ...`, with the arena use), an arena that fills is
reported, and the `rt` line counts interpreted shader runs (`interp`).

## Water and highlights: the reflection LUT as |N.H|^n (2026-11-16)

The user's slot 2 (a pond seen from a cliff) drew the water white; Android Azahar draws it dark
blue with reflections. Ruled out on the way: the hidden 256x256 target at 0x18154F00 is not a
reflection but MH4U's **shadow map** (see below), and the cube-map water code is not used here.
The cause was our own shortcut: the reflection-red LUT (on |N.H|) taken as 1, which turns the
specular_1 highlight into a full-strength glow over the whole surface. With `refllut` (the real
lookup) the water matched Android exactly, at ~2.8 ms more GPU a frame.

`logrefl` dumped the tables: every lit draw in three slots uses the red LUT only, on |N.H| with
abs input, and its entries are exactly x^16 (most materials) or x^40 (a character) at
x = i/256 (worst error 0.0005 / 0.0025 against the PICA's interpolation). So:

- `RasterizerGxm::FitReflectionRed`, on each upload of LUT 6 (cached by content hash): all zero
  -> Dead, all one -> One, a power curve within 1.5/255 at every entry and midpoint -> Power(n),
  otherwise Lookup. Note the LUT's `difference` field is not on the value's scale (11 bits); the
  check interpolates towards the next entry instead.
- `BindPrograms` puts the mode (and n) into spare bits of `fs_config.lighting.lut_rr`
  (`Usse::SetReflMode`, bits 8-10 and 16-23), so the program key, tier 2 and dumps carry it.
  Dead also when every enabled light's specular_1 is black.
- The emitter: Power computes |N.H| (the existing half vector) and squares it - no texture read,
  no LUT index maths; Dead drops the specular_1 term.

Main-target GPU per frame (`scenetime timedraw`, serialised, so relative only):

| | taken as 1 (`reflone`) | Power (default) | lookup (`refllut`) |
|---|---|---|---|
| slot 2, water | 24.9 ms | 26.5 ms | 32.6 ms |
| slot 1, quest counter | 37.6 ms | 40.6 ms | 43.6 ms |

Without timing flags slot 1 shows the same 19-20 fps (diag build) either way, and slot 2 stays at
30 fps in the release build with the water right. Switches: `refllut`, `reflone`, `noreflpow`,
`noreflskip`, `logrefl` (to ux0:data/mh4u/refllog.txt). Further saving if needed (Fable's plan):
inline the half vector instead of parking it in temporaries, and the Fresnel LUT (Schlick-like
ranges) the same way.

### Shadow maps (P5), opt-in: `shadowmap`

MH4U renders a hard-shadow map each frame into 0x18154F00 (256x256): a plain clear quad (white),
then the shadow-mode casters (constant penumbra 0, no alpha test, Z-buffered). Lit materials read
it on unit 0 (orthographic, bias 0x02067B, light 0 and/or the alphas via shadow_alpha). The
backend used to drop the pass and bind white. With `shadowmap`:

- the pass draws into the colour surface itself, with a depth buffer of its own
  (`Surface::ShadowAux`, F32) tested LESS, so the colour keeps the nearest caster; the program
  writes its depth as three bytes (G, B, A = low, mid, high; R, the penumbra, masked off);
  any other scene on that colour (the clear) makes the aux depth start at 1.0 again;
- receivers sample it point-filtered and compare in the program (`ShadowCompare`: one texel,
  border colour as the software renderer has it).

Checked on the console: the map holds the hunter's silhouette (~90 texels) and the receivers
compare against it. It is only character shadows, so it does not affect the water; cost not yet
measured, so it stays off. Diagnostics: `logshadow` (ux0:data/mh4u/shadowlog.txt), `tracecb`,
`dbgshadowblack`, `dbgshadowzero`, `dbgshadowref`, `dbgshadowdepth`, `dbgshadowalways`,
`dbgshadownocull`, `dbgshadowsw`.

## The frame's final brightness grade (2026-11-16)

The user's slot 3 (a vine-covered pillar) and slot 2 looked dark and muted next to Android.
None of the lighting or fog switches moved the brightness; Fable compared the lighting emitter
with the GL generator line by line and found it equivalent, and pointed at the feedback draws we
keep alpha-only. `keepfeedback feedbackbreak` gave the Android picture (slot 3 mean luma 25 -> 39)
at 2-3 fps less. `loggrade` (ux0:data/mh4u/gradelog.txt) showed one such draw a frame, the same
in every scene: a full-screen quad reading the main target 0x180D4800 and replacing it with

    stage 0: t + t -> buffer;  1-3: a contrast curve;  5: lerp(curve, buffer, const5)

and const5 = 0 everywhere seen, so the result is clamp(2 * t): MH4U draws its world at half
brightness and doubles it last. `IsGradeDouble` matches that exact combiner (with const5 = 0) and
the draw is made with unit 0 white and the blend DstColor + One - the target times two, clamped by
the 8-bit store - with no read of the target and no scene break. Slots 1-3 match the
`keepfeedback feedbackbreak` pictures (slot 3 luma 39.1) at unchanged fps (30 / 30 / 24).
Switch: `nogradeblend` (back to alpha-only). A grade with a non-zero const5 would still be
alpha-only; `loggrade` would show it.

## Camp (user's slot 1, 24 fps): pricing and the first cuts (2026-11-16)

Release: game 30 fps, shown 24-25; the render thread waits on the GPU ~1.28 s in 3 s. Main target
GPU (`scenetime timedraw`, serialised) and the experiments from Fable's list:

| flags | ms/frame |
|---|---|
| base | 39.5 |
| `dbgminfs` | 7.0 |
| `dbgminfs dbgminsel_lit` | 8.2 (lit programs ~31 ms of it) |
| `dbgminfs minkeep_tex minkeep_tev minkeep_alpha` | 18.9 (lighting + fog ~20.6) |
| `nofog` | 37.7 |
| `reflone` | 34.6 (the |N.H|^n highlight ~4.9 ms here) |
| `nopvnormal` | 43.3 |
| `dbgconstfr` | 36.7 (the Fresnel fetch ~2.8) |
| `ptoffop` | 34.9 |

Vertex work is ~2.9 ms; render scale is the default (scale_q 4). The camp's lit draws: one or two
directional lights, specular_0 always black, no spot/distance/geometric factors.

- **Fresnel as Schlick** (`Usse::FrMode`, `RasterizerGxm::FitFresnel`): every Fresnel table seen
  is c0 + c1 (1 - |N.V|)^5 (c0 0.109 / 0.010 / 0.101) or flat (1, 0.87, 0), within 0.0001. The
  mode and m ride in lut_fr's spare bits, (c0, c1) in the uniform `lighting_fr_coef`.
  39.5 -> 36.7 ms (the whole fetch); picture unchanged. Switch `nofrfit`.
- **Streamlined lighting** (`nolightfast`): no parking of a pre-normalised directional light's
  vector (N.L reads the uniform), no spot-direction copy without a spot LUT, the power-curve
  highlight multiplied into specular_1 directly. 36.7 -> 36.2 ms.

Shown fps at the camp (diag build, 17 s): 23.6 -> 25.6. Still short of 30: the remaining gap is
~3 ms of GPU. Candidates: the half vector per pixel (two rsq for two lights plus the view
normalise), the alpha-tested discards (~4.6 ms), or a render scale below 1x (user's call).

### Camp, round two (2026-11-16)

- **Per-vertex N.H (`pvhalf`, opt-in):** the generated vertex program computes N.H for one or two
  directional lights into TEXCOORD6.xy (switched by the vertex uniform pv_light_dir[0].w), and
  the fragment program drops the view normalise and the per-light half vector (26 instructions,
  3 rsq). Camp: 36.4 ms against 36.3 without - nothing. `logpvhalf` counts (in game): ~40% of lit
  binds eligible, ~21% software-shaded, ~35% without a red LUT. Kept opt-in.
- **x^100 reflection table** now takes the power form too: between entries the exact curve may
  differ from the PICA's straight line by 2.7/255 next to the peak, so midpoints are checked
  against 3.5/255 (entries still 1.5/255). No measurable change at the camp.
- **Software-shaded lit draws read a quaternion as their normal:** the pass-through vertex
  program copies the quaternion into TEXCOORD5, which per-vertex-normal fragment programs read
  as (normal, N.V). Such draws (~21% of lit binds at the camp) now get a fragment program that
  builds its normal per pixel (`Usse::SetNoPvNormal`, lut_rr bit 12; `pvnormalsw` restores the
  old reading). No visible change in slots 1-3, +0.2 ms.
- **Punch-through (E1):** base 36.2, `ptoffop` 32.0, `prepass ppskipb` 32.9, `prepass ptskipop`
  31.1 (`prepass` alone gave no reading). Dropping the cut-outs saves 5.1 ms, more than making
  them opaque - not just honest visibility here, so ordering (E2) and always-passing tests (E3)
  are worth measuring next.
- **E3 (`ptsources`):** the camp's unblended alpha-tested draws read alpha from texture unit 1,
  ETC1A4 (GREATER 128 / 0 / 5 / 30 / 80): textures with real 4-bit alpha, so no test is provably
  always passing.
- **E2 (`ptorder`):** every camp frame on the main target is `O2 o2 B4 O56 P23 B22 o1 B33` -
  all opaque draws come before the first cut-out (0 opaque vertices after it). Deferring the
  cut-outs cannot help: their cost is their own shading of overlapping cards. Item 2 is closed.

### Camp, round three: exact trims (2026-11-16)

- **Black lighting terms** (`Usse::SetBlackTerms`, spare bits of lut_d0; `noblackterms`): a
  light's black specular_0 or ambient, or a black global ambient, is neither loaded nor added
  (MH4U's world: specular_0 and ambient always black). Camp 36.6 -> 35.1 ms - more than the
  instruction count suggests.
- **The grade's program** (`nogradeprog`): the frame-doubling grade, drawn as a blend, now uses a
  combiner of constant white (stage 0 constant checked to be white) instead of the game's six
  stages with a texture read. The draw itself costs ~1.4 ms (`dbgnograde` drops it, diag only);
  the program change takes ~1 ms of it. Pictures identical.

Camp main target now ~33.9 ms (run-to-run noise ~1 ms). Release, 25 s: camp 27.8 fps (was 26.1),
pond and pillar 30.0.

### Camp, round four: where the lit time is, and registers (2026-11-16)

Pricing (main target, serialised): base 34.0; `dbgminfs` 7.1; `+minkeep_tex` 11.7; `+tev` 15.8;
`+alpha` 17.3; `+light` (no alpha) 28.3; `dbgminsel_unlit` 34.0 (unlit costs nothing);
`dbgconstfog` 35.0; `reflone` 27.7; `pvhalf reflone` 28.0; `ptoffop` 30.1; `nomipmaps` 34.3.
New class selectors (`dbgminsel_bump`, `_flat`, `_l2`, `_kill`): stripping the lit programs
without a normal map leaves 14.3 ms (they hold ~19 ms), with one 26.6, two-light 29.3,
alpha-tested 28.1. `logtemps` (templog.txt) listed the camp's lit programs: 28-64 temporaries,
most with one or both lights loaded from memory (24 temporaries of TmpLightMem each).

- **Shadow literals only where used:** today's shadow constants (8 SAs) had joined every
  program's literal table; now only shadow compare / shadow rendering programs carry them.
- **Compact resident lights** (`nolightcompact`): a resident light declares only the members
  the program reads (MH4U's directional lights: specular_1, diffuse, the direction - 12 SAs
  instead of 24), so two lights stay in registers. Memory lights at the camp: 8 of 103 lit
  programs (was most); the usual program now 28-40 temporaries.

Camp: 34.0 -> 33.4 (literals) -> 32.8 ms (compact lights) - smaller than the temporaries
suggested. The highlight power chain still costs ~6 ms (`reflone`), unchanged by `pvhalf`.

### Textures as non-dependent reads: no gain (2026-11-16)

Tried for texture unit 1 (`texquery1`, removed again). Two console GPU crashes ("render gpu
crash", the second powered the Vita off) were the descriptor writer setting 0x800 on every
query: Fable compared TuizaVita's psp2cgc-compiled two-query programs (reserva_r_f /
reserva_r_fl), where 0x800 is "last texture query" and sits on the last query only, and
0x02000000 marks the last iterated input. The writer now sets 0x800 on the last query alone
(single-query programs, the fog path, are unchanged). With that the query ran, but the camp
measured 33.3 ms against 32.8 without, and some draws read white (the hunter's weapon, a
fence) - not pursued. Texture latency is not what limits the lit programs.
Proven query carriers in Sony's programs: COLOR0 and four-wide TEXCOORDs, and a standalone
query descriptor (input 0xF, no 0x0c000000); no two-wide or own-coordinate carrier.

### Camp: the GPU time outside the main target (2026-11-16)

`presenttime` (diag) times the GPU blit scenes and the frontend's present scene on their own:
the display-transfer blit (main target 0x180F0800 256x512 RGBA8, cropped, into one of three
240x400 RGB565 framebuffers) ~0.8 ms a frame; the present scene ~3.1 ms with the sharp-bilinear
upscale, ~1.2 ms with plain linear (`upscale=0`). So a frame at the camp is ~31 ms main target +
0.8 + 3.1 = ~35 ms, which is the 28.7 fps shown. `logxfer` logs the first display transfers
(xferlog.txt). Presenting straight from the main target would save the blit, but the present
marks its source busy until the frame's finish, so the next frame's first draw into the main
target would wait - not tried. The sharp-bilinear cost is its dependent read at 960x544.

### Upscale filter at the camp (2026-11-16)

Release, camp, 25-30 s each: linear 30.0 fps (0 s under 29), sharp bilinear 29.0-29.5, and a
pre-scaled sharp variant (2x nearest-neighbour into its own 480x800 target, then a linear
stretch; tried and removed) 29.0 with 6 s under 29 - its extra scene costs what the
non-dependent read saves. The user's default is now linear (`upscale=0` in video.txt).

## HUD text: a half-pixel offset (2026-11-16)

The user's photo of a real 3DS shows MH4U's HUD text crisp at 400x240; ours read "(No Itarr)"
for "(No Item)". The surface dump (`dumpfile`, main target 0x180D4800) showed the letters already
malformed at native resolution, before the stretch, so not an upscale matter. Mipmaps off: no
change. `dbghudpoint` (point sampling for the 2D layer): no change. `dbghudhalf` (the 2D layer's
viewport moved half a pixel): crisp letters. With the shift on every draw (`SyncState`), the
world is the same picture half a pixel over and the text is right - the PICA and GXM differ by
half a pixel in where a primitive's pixels sample, and one-texel-per-pixel glyphs were read
between texels. Now the default; kill switch `nohalfpixel` (flag or file). Release: camp 29.5,
pond 30.1, pillar 30.0 fps; menu text crisp.

The 2D layer (post-grade draws): `dbgnohud` / `dbgonlyhud` split it cleanly (it costs ~2.8 ms at
the camp); `hudlog` lists one frame of it: ~32 software-GS sprite draws (SrcAlpha /
OneMinusSrcAlpha, no depth) and two full-screen quads. Fable's plan for replaying the sprites in
the present at screen resolution is kept for later if the native text is still not enough.

## Sharp HUD text: the text replayed at screen resolution (2026-11-17)

Even with the half-pixel fix the 400x240 text stretched 2.4x stays soft (the user compared a
video of a real 3DS; a 1.5x render looked good, 1.25x barely better). Stage 0 at the camp
(scenetime, merging off): ~99 post-grade draws a frame; dropping the GS sprites -1.6 ms, the two
full-screen quads -0.2, the alpha-only draws -0.5 (not dead: draw #66 blends with DstAlpha on
the RGBA8 target, so the planned "dead draw" drop was cancelled). Re-drawing every HUD sprite at
the display's resolution would cost ~3-5 ms, so only the text is moved:

- **Which draws:** MH4U's font is a 1024x1024 A4 atlas (0x20AA3800), drawn as software-GS sprites
  with a constant colour. The IA4 atlas beside it holds icons (found by shifting the replay:
  `dbghudshift` moves the replayed draws 30 px).
- **Record** (`RecordHudDraw`, from the software draw loop in `DrawTriangles`): the bound
  pipeline, units 0-2, the last reserved fragment uniform buffer (its size from the program),
  and the triangles copied out of the ring - instead of drawing. While the 2D layer is open the
  repeat, begin-repeat and texture-only fast paths are bypassed so every draw reaches it.
- **Pair:** at the display transfer the frame's records go with its output framebuffer
  (`hud_ready`, by address); the present's top screen carries its framebuffer address.
- **Replay** (`ReplayHud`, hook `ReplayHudText` called by EmuWindowVita::DrawScreens after the
  top screen's quad): the same pipelines in the present scene, positions mapped from the 2D
  layer's clip space to the screen rectangle (x from -NDC y, y from -NDC x: the present's turn),
  pass-through vertex uniforms with z = w / 2, the fragment uniforms copied back.
- **Option:** pause menu "Sharp HUD text" (`hudtext=1` in video.txt; `hudtext` flag in diag).
  Diag: `dbghudboth` (native and replay together, vertex colour tinted red - icons only),
  `dbghudshift`, `hudtexticons` (also the IA4 icon draws), `hudtextlog` (counts), `hudsx`/`hudsy`.

Release with it on: pond 30.0, pillar 30.0, camp 28.8 (4 of 20 s under 29; 30.0 without) - the
camp has no margin, so it stays an option. Text crisp in the HUD and the in-game menu.

Village (user's slot 4, Val Habar with the minimap): render-thread bound (GpuThread ~990 ms/s,
speed 84-90%). The first Sharp HUD text bypassed the draw-merging fast paths for the whole 2D
layer: 26.5 -> 23.3 fps there. Now only the text draws (the A4 font in the layer,
`HudTextDraw`) take the full path, and nothing repeats a recorded draw: 25.7 off, 24.9 on.

Sharp HUD text made cheap (2026-11-17): the frame's text goes into pooled buffers (vertices,
uniforms; four reusable framebuffer slots, swapped at the display transfer so no allocation per
draw or frame), consecutive text draws with the same pipeline, textures and uniforms are merged
into one run, and a text draw that repeats the recorded one before it takes the cheap repeat path
and joins the run in the recording (as native glyph draws join the deferred run) instead of the
full draw setup. Village, slot 4 (render-thread bound): off 25.9 / 26.5, on 26.0 / 26.1 fps -
within noise. Note the village spot itself is ~26-27 in every build tried (this morning's
eaae5bd05: 26.5 / 27.3; the user sees 27-30 across Val Habar, lowest where most NPCs are).

Sharp HUD text under later UI (2026-11-17, slot 5: the Item Box opening over the Item Pouch
left the pouch's text on top): replayed text is drawn last, above anything the game drew over
it. Each recorded run keeps its extent; a later non-text draw on the 2D layer is checked against
the runs still pending, and any it overlaps is drawn natively right there, before it (in order,
so covered as on hardware; that text loses the sharpening). Shipped draws (no extent known) emit
all pending runs. Village counters (hudtextlog): 4 runs, ~40 checks, 1 native run a frame.
Village fps with the check: on 25.7 / 25.8, off 26.5 (within the scene's run-to-run spread).

**The `timedraw` switch file was left on the card** (ux0:data/mh4u/timedraw, release per-phase
timing: clock reads per draw). Val Habar slot 4, Sharp HUD text on: with it 25.7 fps (render
thread ~960 ms/s); without it 29.3 / 30.0 (render thread ~815-875 ms/s). Every village figure
above this note (and the "village was already ~26" bisect) was measured with it on. Renamed to
timedraw.off on 2026-11-17; check the card for it before any fps measurement.

**Sharp HUD text removed (2026-11-17).** With the Heavenfall mod's HUD at 100% the native text is
readable, so the option (recorded HUD text replayed at screen resolution) went: the code of
58c3ea0d7..f834922b7 reverted. Release, Sharp HUD text off, mod HUD 100%, game fps over two passes
(camp slot 1 / village slot 4): current code 1x 30.0/30.0 and 30.0/29.7, 1.25x 29.2/29.5 and
29.9/30.0; code removed 1x 29.9/29.9 and 29.9/29.3, 1.25x 29.7/29.6 and 30.0/29.9 - the same
within noise (render thread ~550 / ~820 ms/s in both). Disabled, it cost nothing measurable.
The half-pixel viewport fix stays: without it (`nohalfpixel`) fps is the same (1x 30.0/30.0,
1.25x 29.5/29.4 camp) but "(No Item)" reads "(No Itarr)" at both scales.

**Slot 6 (hunting area, 2026-11-17): GPU-bound, ~36 ms on the main target at 1x (diag
scenetime), ~140 draws a frame.** Fog costs ~2 ms (`nofog` 34.3 against 36.1); `dbgfognoinput`
(new: fog arithmetic with a fixed factor, no per-pixel input) 36.1, so the fog query is free and
fog adds no temporaries (logtemps, 22 program pairs identical, +7 instructions). `dbgconstfog`
and `nofogstrip` were missing from the program cache key, so readings with them before this date
used the normal cached programs; they are keyed now (FogDiagKeyBits). The spot is live gameplay
(monsters attack, the camera moves, menus open), so A/B readings there swing by 5 ms; a still
save state is needed before pricing alpha test and shadows there.

**Power highlights were never selected (fixed 2026-11-17).** `BindPrograms` judged "red only"
from the raw enable bits of the green and blue reflection tables; MH4U's materials are lighting
Config1, where those tables do not exist, and leave the bits set - so every lit program took the
LUT lookup (`logreflwhy`, new: `config 1 ... rg 1 rb 1 supported 0 | only_red 0 | fit 1 n 16 ->
mode 0`). Now green/blue count only where the configuration supports them (as the emitter's
LightingLutEnabled), and Power also requires the table's input to be N.H (Fable's review).
Slot 7 (hunting area), diag scenetime: 36.7 -> 30.6 ms (`refllut`, the old path: 36.7/35.4;
`reflone`: 27.8). Frames (dumpframe) against the lookup: mean difference 0.20/255, 0.18% of
pixels over 24 (the stats overlay). Earlier notes that "the power chain costs ~6 ms, unchanged by
pvhalf" measured lookup programs; pvhalf now becomes eligible and is worth re-measuring.

**Intro movie on the v1.0 executable (2026-11-17).** The movie-decoder poll patch
(Mh4uPatchFmvPoll) checked the v1.1 addresses only, so with a Heavenfall mod from the web
installer (built from the base game, so the game runs v1.0 code even with the update installed)
it logged "code differs, not patched" and the intro was back to the slow polling. The same code is
0x698 bytes lower in v1.0 (branch 0x9680CC, return 0x968114, the loop's return 0x963AE4; byte
match on 24 bytes around the branch and 32 around the caller); both are tried, each checked word
by word. Same for the audio filter HLE (0x1A4 lower) and the 30 fps cheat (pointer 8 bytes lower).

**Crash: "No more processes" in an FS read (core dump 2026-11-17 19:38).** The emulation thread
aborted: std::terminate from Service::FS::File::Read, the exception a std::system_error, code 11
(EAGAIN, newlib's "No more processes"), found from its typeinfo pointer in the dump. Reads of
64 KiB and more go through HLERequestContext::RunAsync, which starts a thread per call with
std::async; when the Vita could not create one (most likely no memory left in the partition for
its stack) the throw was outside every handler. Fix: RunAsync now queues every async section
(FS reads, AM's work) on one shared pool, `HLE_Async`: two long-lived threads with 256 KiB stacks,
made on first use (Kernel::AsyncSectionPool), and used only if both threads were really created
(NamedThread fails silently on the Vita, so ThreadWorker::AllStarted checks). Without a whole pool,
the old thread-per-call path runs, and if that throws, the section runs on the emulation thread.
Console: boot, intro, the save loaded into Val Habar at 30 fps.
Tooling: the dump is gzip'd ELF; a stdlib reader (thread notes, registers, stack scan) plus
arm-vita-eabi-addr2line on build-vita-rel/src/citra_vita/citra_vita gives the stack.

**pvhalf with the power form (2026-11-17, slot 7, diag scenetime with tier2now):** base 31.7-32.9
ms, `pvhalf` 29.6-30.3 ms (~2.3 ms). Frames against the per-pixel half vector (dumpframe): mean
difference 13.7/255, 23% of pixels over 24 - the ground's highlights shift across wide areas. Not
an invisible change, so it stays an opt-in switch.

**Two exact lighting trims (2026-11-17, slot 7, diag scenetime with tier2now):**
- *Spot direction only where used* (`nospottrim` restores the old code): the spot table's enable
  bit is always set, so every light carried and copied its spot direction; the table is only read
  for a light with spot attenuation (MH4U's lights have none).
- *The power form without the normalised half vector* (`noexactpow`): for an even exponent,
  |N.H|^n = ((N.Hu)^2 / (Hu.Hu))^(n/2) with Hu = nview + L - no square root, no half vector
  parked and reloaded; used when nothing else reads the half vector.
Old code 32.9 / 32.8 ms, both trims 31.2 / 30.6 / 31.1 ms (~1.9 ms), each alone ~1 ms. Frames
against the old code: mean difference 0.29/255, the changed pixels all on the animated characters
(the two frames are moments apart); terrain identical.

**Program cache versioning (F2):** programs.bin's version hashes the emitters, the shader
generator, the PICA headers and gxm_program_epoch.h, no longer the whole of gxm_rasterizer.cpp,
whose edits reset every player's cache for nothing. The epoch file says when to bump it.

**Per-vertex highlights as a setting, tried and removed (2026-11-18).** Shipped briefly as a
pause-menu option (83e4d6ddf). Playtest: no practical gain (sharp bilinear fits without it too;
1.25x still too slow), and the water by the camp's pond (slot 1 area) turned fully white - the
water material's highlight does not survive per-vertex N.H. Reverted; `pvhalf` is a diag flag
again and would need that water fixed before any further use.

**Dense vegetation (slot 1, 2026-11-18): 8-11 -> 28 fps shown.** A new hunting area with dense
grass: game 8-11 fps, speed 26-36%. Release counters: ~1,200 point-mode GS draws a second (33k
input vertices, 66k triangles, 0 memo hits); the pre-shade worker saturated (VS ~300 ms, GS ~580
ms a second), the render thread yield-spinning behind it (`rt ops`: sw 930 ms of which wait 819),
the GX thread braked (`wait b` ~800-1000 ms). `hwgs` (GPU GS) already removed the CPU work from
the render thread, but the worker still shaded every point draw the GPU took (QueuePreshade marks
all of them, and the op never cancelled it), so frames still waited on it: game 30, shown ~10.
Fix (Fable's plan): when AccelerateGsDraw takes a draw, the op moves it Pending -> Stolen and the
worker skips it. GPU cost of the GPU GS here: 34.4 -> 38.5 ms (diag scenetime). Release: hwgs +
fix 26.0 / 27.7 shown, speed 88-99%; slot 7 (GPU-bound hunting area) 30 either way. `hwgs` is on
by default now; `nohwgs` (flag or ux0:data/mh4u/nohwgs) restores the software path. Default
release at slot 1: game 29.8, shown 28.0, speed 99%. Next candidates from Fable's ranking: four
indexed corners per point instead of six (a third less fused vertex work), merging consecutive
GS draws, a CPU/GPU hybrid split, a cheaper CPU GS (emit path, JIT).

**Crash in the pause menu (core dump 2026-11-18 10:53):** the render thread aborted on ImGui
1.79's `IM_ASSERT(g.NavWindow && result->Window)` (imgui.cpp:8873; strings and line read from the
dump's registers). The SDK's prebuilt imgui keeps its asserts. A pad move is applied a frame after
it is requested; the menus swap whole windows between frames (pause -> save states -> the status
window), so a direction landing on a switch left the move pointing at a window that was gone.
Ui::Update now cancels a pending move whose window was not drawn last frame (CancelStaleNavMove)
before NewFrame. Stress test (directions pressed together with Circle/Cross on every switch, three
rounds): no crash.

**GPU GS corners indexed:** a point's four distinct corners are written once and drawn through a
16-bit index list (`nohwgsidx`: six copies as before). Slot 1: 42.6 vs 42.7 ms - the vertex stage
was never the limit there - but a third less ring data and vertex work.

**Slot 1, second view (GPU-bound, 2026-11-18):** game 30, shown ~24. Diag scenetime 42.4 / 41.6 /
42.6 ms (vertex 4.7); `ptoffop` 17.3, `ptoffbl` 39.9, `nofog` 40.8, `dbgopaque` 41.9,
`dbgshadowzero` 42.7, `reflone` 42.9, `nohwgs` 42.8, `dbgminfs` 7.4. ~25 ms is the unblended
alpha-tested draws (the grass cut-out cards). Fable: the existing `prepass` never reached GS draws
(AccelerateShippedDraw only), and the kill comes after lighting and the combiner.

**Grass cut-outs: the depth pre-pass reaches the GS draws (2026-11-18).** Fable: `prepass` was only
wired into AccelerateShippedDraw, so the GPU GS cards (the grass) never had it. Wired into
AccelerateGsDraw (both passes keep current_gs). Slot 1, diag scenetime: base 41.7 / 42.0;
`ptoffgs` (new: alpha test off for GS draws only) 21.0 - the grass cards are ~21 ms; `prepass`
33.7; `prepass ppskipb` 26.9 (the depth pass alone ~9 ms over no grass); `prepass ptskipop` 17.7;
`gsrev` (new: points in reverse) 39.8 - card order is worth ~2 ms; grass reduced to texture +
combiner + kill 31.3. The pre-pass is now on by default for GPU GS draws only (`nogsprepass`
turns it off; shipped draws keep the opt-in `prepass`). Release: slot 1 shown 24 -> 29.7, game
29.7, speed 99%; slot 7 30.0 unchanged. Left on the table: the depth pass's ~9 ms (front-to-back
card order, an earlier kill in the emitter).

**Slow intro after the game's own Quit, after a save state (2026-11-18, user report).** A save
state carries the code with the movie-poll patch already applied (`svc 0xF0` at the branch).
After a state load Mh4uPatchFmvPoll saw that word, logged "code differs, not patched" and gave
up, so the svc ran without its handler (the v1.0 shift of a web-installer mod lost) and the movie
decoder spun: after Quit Game the intro alternated 100% and 7-20% speed (diag). The check now
accepts the already-patched word. Release, same sequence (slot 2, Quit Game from Val Habar):
100% / 30 fps for the following minute. The glitched line at the bottom of that intro is a
separate issue, not yet looked at.

**Power-off loading slot 7 (2026-11-18), caused by a4824a3c5.** That fix re-wrote the movie-poll
svc and synced its code page right after a state load; loading slot 7 (a state from the previous,
v1.1-built mod) then switched the console off, twice, no core dump. The same build without the
fix loaded it at 30 fps. Now an already-patched poll only sets the shift (no write, no SyncCode).
Slot 2 quit intro 100% / 30 fps; slot 7 loads at 30 fps.

**Noise line at the bottom of the intro after Quit Game (2026-11-18).** The half-pixel viewport
shift (2026-11-16, for crisp HUD letters) moved every draw on both framebuffer axes. The top screen
is stored rotated, so framebuffer x is the screen's vertical: the x half made the movie quad's last
row sample half a texel past the frame. On a fresh boot that memory is zero (black, invisible);
after gameplay it holds leftovers, hence a line of noise. Tested on the slot 2 quit sequence:
x-only shift kept the line, y-only removed it, and the HUD letters are as crisp with y alone
(slot 7). The shift is now y-only; `halfpixelxy` (diag) restores both axes, `nohalfpixel` still
turns it off. A HUD-only scope did not work: the movie frames also run the grade pass.

**Bug report (pause menu, 2026-11-18).** `src/citra_vita/bug_report.cpp` writes
`ux0:data/mh4u/reports/mh4u_report_<date>_<time>.zip` (its own small zip writer on zlib; save
states stored, the rest deflated at level 1): report.txt (app version, the game program the movie
patch recognised, mod and data folder listings, video.txt, gxm_flags.txt, the cheat file, the last
240 `[mh4u] speed`/`cpu` lines), the in-game save, extdata, the newest crash dump, optionally the
newest state. The speed/cpu lines go through `ReportPrintf`: a fixed 240 x 512-byte ring, no
allocation, nothing on the card until a report. The newest 3 reports are kept. On the Vita: 0.9 MB
in ~1 s, 28.7 MB with a state in ~25 s; the game resumes at 30 fps.

Start-up freeze, not fixed: three times on 2026-11-18 the app stopped right after "RunTitle:
entering run loop" (no speed line ever printed) and the console switched itself off ~100 s later,
no core dump, battery 76%. The same start-then-disconnect shape is in the event log on earlier
days too. The slot 7 power-offs blamed on a4824a3c5 were this, not the state load.

## Out of heap after 1 h 45 min: fragmentation, not a leak (2026-11-18)

A user session (fishing at the camp) ended in `std::bad_alloc` on a 258 KiB request: the audio
thread decoding an ADPCM buffer (`Source::DequeueBuffer` -> `Codec::DecodeADPCM`); the dump has
`typeinfo for std::bad_alloc` on the stack. The console log of that session (188,400 frames)
shows in-use heap flat, 194 -> 212 MiB (growing with pipelines 65 -> 2886), while the arena
grew 208 -> 245 MiB, the whole 240 MiB block: ~33 MiB free, in pieces too small for 258 KiB.

`heapprof` now also measures fragmentation, every 10 s:
- `heapfrag`: arena, used, free bytes and chunks, the largest free chunk, the top chunk, free bytes
  by chunk size (a walk of newlib's free lists under the malloc lock);
- `heapbig`: every allocation of 32 KiB or more (exact, not sampled) by call site: bytes, count,
  frees and largest size since the last report, and how many are still live;
- a failed allocation prints its size and the free-list numbers even with the profile off;
- reports are appended to `ux0:data/mh4u/heapprof.txt` (kept open, one write per report: opening
  it each time cost ~150 ms of game time). A report takes ~6 ms.

First look (slot 1, 1 min): the audio sink callback allocates and frees a 32 KiB buffer ~68
times a second, and `Service::FS::File::Read` allocates buffers up to 685 KiB per read; both are
churn that can leave holes for long-lived objects to pin. Next: a long user session with
`heapprof`, then plan the fix from where the free space ends up.

**Fix, first batch (BigArena).** `src/common/big_arena.{h,cpp}`: an 8 MiB block taken from the
heap at start-up (kill switch `ux0:data/mh4u/nobigarena`), 4 KiB granules, best fit with
coalescing, one mutex; requests under 32 KiB or that do not fit go to malloc. Routed through it:
the GPU command list snapshots (`GPU::QueueCmdList`), decoded audio (`StereoBuffer16`'s vector)
and file read buffers (`File::Read`). The time stretcher pops into one persistent buffer instead
of a new 32 KiB vector per callback. (Its float conversion vectors are unused here: the Vita's
SoundTouch is built with integer samples.) Slot 1, 2 min with `heapprof`: the `heapbig` churn is
gone (0 KiB a window, from 15+ MiB), arena peak 1.5 of 8 MiB, no fallbacks, 30 fps. The health
line now ends with `bigarena used/capacity peak largest fallbacks`.

heapprof: the pointer table now deletes by backward shift (tombstones had filled it after an hour
and stopped sampling), has 65536 slots, prints `dropped` samples, and lists `heapgrow`: the sites
whose live bytes grew most since a baseline taken at two minutes.

**Fix, second batch.** The geometry-shader memo records into one scratch vector kept by each
`PicaCore` (trimmed above 1 MiB) and stores exact-size copies: no more doubling churn on every
miss, and entries no longer carry up to 2x spare capacity. The A32 shader JIT's label table
(4096 labels, 80 KiB) is one shared compile-time scratch instead of a member every compiled
shader kept; `Run` reads a compact offset table for the compiled span. A 2 MiB emergency reserve
(`HeapReserveInit`) is freed on the first failed `operator new`, which is then retried and logged
(`heap: OUT OF MEMORY ... emergency reserve released`). Slot 1: memo hit/miss pattern unchanged,
same shader JIT compiles, 30 fps, picture unchanged.

## Data hooks from the MH4UltimateDC research (2026-11-19)

Plan: `plans/vita-mh4u-decomp-perf-hud.md`. New `src/core/arm/vita/mh4u_hooks.{h,cpp}`: once a
guest frame (from `RunSlice`, the guest stopped) it reads, and for opt-in switches writes, the
game's own objects - data only, no code patched. The executable is recognised by a vtable word
(USA v1.1, USA v1.0, EUR v1.1); states made from another executable switch it (slot 7 is v1.1,
the card's game is the web installer's v1.0). `Mh4uHooks::BeforeSave` puts the game's values
back before a state is written. Kill switch `nohooks`. Also: `GuessCmdRenderProperties` (the
right-eye check, on the emulation thread) now parses into a list of its own instead of resetting
the one the GX thread may be parsing; `nobottomskip` as a file now also keeps the bottom draws in
release (it kept only the transfer).

New on the release `cpu` line: `re l x t` (second-eye lists and transfers refused, top-sized
transfers), `bot d x` (bottom draws and transfers dropped), `fx` (post-process draws dropped).
Logged on change: `render word live/edit`, `filters (n): ...` (the scene's unit list, class
names read from the game's type info for unknown entries).

**P2, second eye: nothing to gain.** Slot 1 and slot 7: `re l0 x0 t30` - no second-eye list or
transfer at all, 30 top transfers a second. Bit 0x40 stays set in the live word and does nothing
here. Closed.

**P3, filters:** the scene list at `sScene+0x20` holds all scene units, not only filters: in the
slot 7 hunt it was lights (`uInfiniteLight`, `uCameraLight`, `uHemiSphereLight`), a shadow
(`uUniformShadow`), fog (`uColorFog`) and two colour corrections - no blur, haze or bloom
(bloom is off through the mod). Radial blur should appear with a roar: the log is on in release
to catch it in a play session. `fx 60` (two dropped bloom/DoF draws a frame) in slot 1, `fx 0` in
slot 7.

**P1, bottom render pass off at the source (`srcbottom`, opt-in):** the game builds ~6,800-7,200
bottom-screen draws a second in Single screen (`bot d`), all parsed and then dropped. With byte
`sLtRender+0x8AB2` held at 0 the game builds none (`bot d0 x0`):

| | Slot 1 base | srcbottom | Slot 7 base | srcbottom |
|---|---|---|---|---|
| Emulation thread CPU ms/s | 719 | 621 (-14%) | 682 | 605 (-11%) |
| GX thread wall ms/s | 382 | 334 (-13%) | 324 | 270 (-17%) |
| GPU thread CPU ms/s | 593 | 566 | 474 | 455 |
| Render ms/s | 594 | 572 | 510 | 470 |

30 fps both ways (these scenes are GPU-bound); picture identical (screenshots). Still to test
before it becomes the default: the menus with previews (item box, equipment, guild card, map,
quest counter), the software keyboard, HOME, switching layouts, saving and loading states.

The pause menu opened by itself over a garbled frame right after a state load in one of these
runs (and once on 2026-11-18): the spurious pause after loads, not these changes.

**New save states (2026-11-19).** The user remade slots 1-7 (map in
`plans/vita-mh4u-decomp-perf-hud.md`, 1.4). With `srcbottom` all seven load and run (`bot d0`);
the item box opens and stores items (slot 5), the Gallery cutscene plays (slot 6), the quest
counter accepts a quest (slot 7). Slot 2 (heaviest field): 9,330 bottom draws a second without
it; emulation thread 721 -> 616 ms/s (-15%), but shown fps stays ~28: there the pre-shade worker
(~900 ms/s) and the render thread (~1,000 ms/s) are the limit, not the emulation thread. Slot 7
(the quest counter menu open) runs at 23 fps with and without `srcbottom` (GX ~960 ms/s): the
menu itself is slow (see `plans/menu-rendering.md`).

Test-harness note: vita-agent replays an identical upload (same file, same path) from its
operation cache without writing, so re-creating a switch file after deleting it needs new
content.

**`srcbottom` is the default** (kill switch `ux0:data/mh4u/nosrcbottom`; `nobottomskip` keeps the
whole bottom screen). The user's checks with it on: equipment box and appearance previews show,
guild card and map work, the chat keyboard works (at ~20 fps, as at character creation), PS button
and back works, switching to two screens brings the bottom screen and touch back, saving to slot
8 and loading it works. One crash loading slot 7 after those menu tests, in the state loader's
teardown of kernel objects - not this change (open bugs in the TODO).

**G2, Smooth gradients (pause menu, `video.txt` `smooth=1`, off by default; kill switch
`ux0:data/mh4u/nofb8` for the 8-bit copy).** A top-screen display transfer (240x400 RGB565,
unscaled) is blitted into a renderer-owned RGBA8 copy instead of the cache's 5:6:5 buffer
(`RasterizerGxm::SmoothDisplayTransfer`, four slots keyed by the guest address), and the present
shows it (`AccelerateDisplay`). Fills, texture copies, other transfers and CPU writes over a
slot make it stale (back to the cache's buffer); guest cache flushes do not, as for any
GPU-written surface. The hook module clears the dither bits (0x100000 top, 0x200000 bottom) in
the render word's edit copy while on, puts them back before a save, and gives up with a log line
if something else keeps setting them. On the user's install the mod's "No Dithering" has the top
bit clear already (`000F036E`), so the option only removes the 5:6:5 banding there. Slot 4
(village sky) and slot 6 (Gallery cutscene and its fades): pictures correct, 30 fps either way.
The banding difference needs a lossless capture (vita-agent captures JPEG) or a look on the
console.

User check (2026-11-19): with Smooth gradients on, the bands in the sky are gone. Sharp bilinear
makes the HUD much less blurry, but the text stays low-resolution; the old Sharp HUD text
(replayed glyphs) read much better, apart from its mixed sharp/soft text in menus such as the
Quest Counter. So the alpha-mask HUD of the decomp plan (which would only equal Sharp bilinear on
the HUD) is dropped; next is the whole 2D layer drawn at screen resolution, as an option
(`plans/vita-mh4u-hud-hires.md`).
