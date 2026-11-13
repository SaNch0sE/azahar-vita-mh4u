# Monster Hunter 4 Ultimate on the PS Vita: TODO

The port is game-specific: an optimisation may break behaviour other titles rely on.
Measurements and history are in `vita-mh4u-performance.md`.

## Performance (in order)

1. **Per-vertex lighting for the lit world.** One fragment program (`25bbe5f060`: lighting
   config 1, one directional light) is ~46% of the GPU's fragment time on the ship's deck;
   106 of its 199 instructions are lighting. The deck has ~2 pixels per vertex, so lighting
   inputs computed per vertex and interpolated should look the same. Now headroom rather
   than a necessity: the deck is ~32.9 ms of GPU a frame with item 2.
2. Done: the reflection LUTs are taken as 1 by default (`refllut` restores them): deck GPU
   ~35.5 -> ~32.9 ms a frame, shown fps 30-31. Close-up highlights are a little brighter;
   a per-vertex reflection term would bring the shaping back if it matters.
3. **Hardware-filtered lighting LUTs** with one wait for all lookups (check the console can
   filter the chosen format; signed tables need care).
4. **F16 colour pipeline** in the fragment emitter (TEV, texels, lighting sums): the largest
   remaining lever, and the riskiest (known four-wide miscompiles on the console).
5. **Render thread:** the ~18% of geometry-shader glyph draws that change state pay a full
   draw setup (~150 us); a partial update would help character select most.
6. Done: the bloom / depth-of-field chain and the shadow-target clear are dropped by default
   (`postfx`, `shadowtarget` restore them), and colour-only framebuffers draw in the open
   scene (`noscenemerge`). Deck GPU ~37.8 -> ~35.5 ms a frame, shown fps 28-29.

## After the optimisations

7. Done: **single-screen play** with the Heavenfall mod (`vita-mh4u-single-screen.md`); the
   port starts in the Single screen layout.
8. Done: in the single-screen layout the bottom screen's draws (240x320 viewport) are not shipped
   (`nobottomskip` keeps them): ~135 draws a frame on the deck; deck speed with the mod 82-86% ->
   90-93% (27-28 fps).

## Open bugs

- Lit draws sent through the software vertex path (`nohwvs`, or the `litsw` experiment) hang
  the GPU at the first lit frame.
- The cutscene's colour grade on the depth-of-field focus pixels is lost (feedback draws are
  drawn alpha-only).
