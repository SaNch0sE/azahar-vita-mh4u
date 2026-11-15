# Plan: MH4U UI screens to 30 fps (Fable, 2026-11-16; nothing built yet)

Measured (release, timedraw): name-entry keyboard 22 fps, ~11,000 GS draws/s (~500/frame,
1-2 points each); worker ~85 us/draw (apply ~18, setup ~8, VS+GS ~48), render ~64 us/draw plus
~26 us waiting, GX ~45 us/draw plus ~400 ms/s on the queue brake. Supply box 27-28 fps
(~5,300 GS draws/s, also ~290 ms/s end-of-frame GPU wait). Budget at 30 fps: ~60 us/draw/thread.

Why consecutive glyphs do not merge: RepeatKey() is fragment_regs_version, bumped by any
fragment-register value change; TexOnlyRepeat forgives only texture bindings; likely per-glyph
TEV constant colour (0xC3/0xD3) and maybe blend/alpha/scissor; A,B,A,B alternation never hits;
draw_epoch bumps from NoteGuestWrite/Invalidate/Flush also break runs.

Order:
0. Diag on the keyboard: repeat_miss[0..5], frag_reg_changes, stat_texonly (key vs epoch
   misses, which registers). Decides R1 vs R2.
W1. Worker fixed costs in RunShippedDraw: skip GeometryPipeline::Reconfigure (make_unique per
   draw) when unchanged; memoize JitEngine::SetupBatch per (code, swizzle, entry); gate
   output_vertex_map.Build/ValidateSemantics on a version; psw timers per draw, not per vertex.
W2. HLE quad expansion for GS 9e6bb9b64d38d710 (HUD): JIT'd VS, then corners in C++, pattern
   0 1 2 1 3 2 into triangle_record; diag gshlecheck memcmp against the JIT path (bit-identical
   or off); kill switch nogshle.
R1. Uniform/state-only repeat: fragment_program_version not bumped by uniform-only registers
   (TEV constants, combiner buffer colour, then per step 0); refresh the uniform block and draw;
   check mode compares FSConfig hash + target key.
R2. If alternation: 4-8 entry fragment-state cache by content fingerprint; split draw_epoch.
F.  AddTriangles(span) feed (one lock, one copy, flips on the worker).
A.  ApplyShipment: classify registers once in CollectState; worker mirror skips LUT/fog/proctex.
G.  Coalesce consecutive GS shipments without fragment/code/GS-uniform deltas (riskiest).

Expected on the keyboard: worker ~45 us/draw, render ~40 us with no wait, GX ~680 ms at 30 fps.

## Findings (2026-11-16, release + timedraw, keyboard slot 1)

- Clean baseline (no timers): keyboard 25 fps (worker 867 ms, render 960 ms, GX 571 ms CPU);
  supply box 27 fps, but there the CPU threads are not saturated: GX waits ~540 ms/s on the
  queue brake and the render thread ~290 ms/s on the GPU at frame end - GPU-bound, separate.
- Repeat merge (diag): ~52% of software draws already merge; every refusal is a key change.
  Changing registers between glyphs: texture unit 0 address/size/format (085/082/08E), the TEV
  stages' sources/configuration (0C0, 0C8/0CA, 0D0/0D2, 0D8/0DA, 0F0/0F2), TEV constants
  (0C3/0D3): the screen alternates between different fragment setups, so R2 (recent-state
  cache) rather than R1 is what applies. Full draw ~65 us: fb 7, textures 15, programs 28
  (config 5, tier2 1, pipeline 5, uniforms 12, other 5), draw 11.
- Worker per draw (~10,800 draws, 1.7 vertices each): register replay ~19 us, setup ~5.4 us
  (was 7.2 before the Point backend was kept across draws), vertex loop ~56 us: attribute load
  2.4, VS 15.7, GS 12.5 us per vertex (timer overhead ~1 us each included). The A32 JIT runs
  the ~150-instruction VS at ~45 cycles a PICA instruction.
- The GS memo cannot help this screen: with skip mode off and a 4096-entry cap it got 0 hits of
  ~7,500 lookups a second (fps fell to 15), also with the key made address-free (pipeline
  base/loader offsets/index offset zeroed). VS uniform rows change only ~100-2,000 times a
  second across ~11,000 draws, so the per-frame difference is in the vertex data or elsewhere
  (the text cursor blinks). Reverted; GsMemoGenerationCap stays 400.
- The game re-uploads a VS program on almost every glyph draw (~720 bytes of upload commands,
  4.7 MB/s of code collected): most of the GX thread's ~1 KB of commands per draw.

Next candidates: R2 (recent fragment-state cache on the render thread), W2 (HLE of the HUD
quad GS, ~12 us/vertex), cheaper ApplyShipment, JIT codegen for the VS.
