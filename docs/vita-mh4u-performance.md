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
| Glyph memo on kept fingerprints, hardware draws keep fragment setup | 64-68% | 21-22 |

The guest now runs at ~39 fps; what is shown is held at ~21 by the render thread (~90% busy, of
which ~20% waiting for the GPU).

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
3. **Ship cutscene depth of field** (mostly fixed). MH4U builds the focus mask with depth-tested
   full-screen quads that colour-grade the frame from the target itself (a feedback read) and
   write a constant alpha. Feedback draws were dropped, which left alpha 1 everywhere and blurred
   every pixel. A feedback draw whose TEV alpha does not depend on the self-read unit is now drawn
   writing alpha only (`nofeedbackalpha` turns it off); the colour grade on those pixels is still
   lost. Still open: close-up shots show blocky garbage where the blur shows through.
   `18070800` is used both as a 256x512 target (two scenes a frame) and as the 64x128 blur
   ping-pong buffer, so the two layouts of that memory get mixed up.
4. **Shown fps is vblank-quantised.** The guest runs ~39 fps, but a frame takes ~47 ms on the
   render thread, so ~21 are shown. Under 33 ms is needed for 30.
5. Low priority: persist tier-2 USSE programs to disk (creating them is already microseconds);
   the render thread's unaccounted ~24%.
