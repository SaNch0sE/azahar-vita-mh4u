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
