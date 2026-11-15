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
