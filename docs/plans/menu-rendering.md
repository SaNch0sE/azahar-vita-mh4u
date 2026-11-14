# Menu rendering: plan to reach 30 fps at the quest counter

Status: plan only, not started (2026-11-14). Written with Fable from the quest-counter
measurements in `docs/vita-mh4u-performance.md` ("Quest counter"); code claims were checked
against `mh4u-vita-fixes` at commit dcccb27e2. Gains are estimates.

Scope: full-screen in-game menus in MH4U on the Vita release build (quest counter first; item
box and smithy follow from the same work).

## 1. Conclusion

- **The render thread is the limit.** It is ~95% busy at ~25 fps, so 30 fps needs at least
  ~17% less work per frame to break even, and ~25% for headroom.
- **The GX thread fits 30 fps once the render queue stops braking it.** Its real work is
  ~620-675 ms/s at 25 fps, which scales to ~750-800 ms/s at 30 fps on core 0.
- **Recommended order:**
  1. Texture-only fast path for software draws, plus a one-line uniform fix (Phase 1).
     Low risk, ~12-17% of the render thread.
  2. Measured GX-side trims (Phase 2). Low risk, ~40-70 ms/s.
  3. Hidden-world skip by a per-frame cover proof (Phase 3). The largest gain (~24% of the
     render thread plus GPU time) and the largest effort; it rests on one MH4U-specific
     assumption, H1 (section 6.6).
- Phase 1 and 2 together may reach 30 fps with no margin; Phase 3 makes it solid. Start
  Phase 3's diag-only discovery step (6.1) alongside Phase 1: its result decides whether
  Phase 3 is buildable as designed.

## 2. Findings from the code that change how the measurements read

1. **The queue brake is inside the GS draw timers.** `GpuThread::EnqueueQueued`
   (`src/video_core/gpu_thread.cpp`) waits inside `ThreadedRasterizer::ShipDraw`, which is
   inside `PicaCore::DrawArrays`. So "pica draw (gs) 506 ms" and "ShipDraw ~30 us" contain
   most of the 264 ms/s brake. Real GS shipping cost is nearer ~20 us per draw, of which
   `ShipDraw` itself is perhaps 12-15 us.
2. **The render queue holds less than a frame.** `GpuThread::MaxDepth` is 512
   (`src/video_core/gpu_thread.h`), the brake releases at 256, and a frame here is ~690
   shipments. The render thread can never see a whole frame ahead, which rules out
   render-side lookahead for the world skip.
3. **A texture change forces a full fragment uniform upload that writes nothing useful.**
   - `SyncTextureUnits` sets `tex_dims_dirty` whenever the bound texture sizes change.
   - `UploadUniforms` treats that as `fragment_dirty`, reserves a new buffer and rewrites
     the whole set (~12 us).
   - The USSE emitter (`src/video_core/renderer_gxm/usse/fs_usse_gen.cpp`) never declares
     `tex_dims` or `fb_dims`; only the unused Cg generator does. Every sprite that switches
     between 256x256, 512x256 and 1024x1024 textures pays for it.
4. **The fragment program does not depend on which texture is bound.** From the texture
   units, `FSConfig` (`src/video_core/shader/generator/pica_fs_config.cpp`) reads only
   `texture0.type` and the wrap modes (bits 8-10, 12-14 and 28-30 of 0x83/0x93/0x9B). It
   does not read dimensions (0x82/0x92/0x9A), addresses (0x85-0x8A, 0x95, 0x9D) or formats
   (0x8E/0x96/0x9E).
5. **The texture memo already covers everything binding-related.** Its key in
   `SyncTextureUnits` hashes the whole `TextureConfig` plus format, so sampler state is
   handled.
6. **`SyncDrawUniforms` reads only two texture-unit fields**: `lod.bias` (0x84) and
   `border_color` (0x81) (`src/video_core/rasterizer_accelerated.cpp`).
7. **MH4U writes 0x25E (triangle topology) ~3.5 times per draw.** Each write leaves the
   assembly fast loop and sets `pa_reconfigure_events`, so every shipment reconfigures the
   primitive assembler on both mirrors.

## 3. Ranked options

| # | Option | Thread relieved | Expected gain | Risk | Effort | Verdict |
|---|---|---|---|---|---|---|
| F0 | Ignore `tex_dims`/`fb_dims` dirtiness when the bound program has no such uniform | Render | ~10 us per texture-size change on the full path; needed by A | Very low | Under an hour | Do first |
| A | Texture-only fast path for software draws | Render | ~110-175 ms/s (~35 us x ~5k misses/s in diag, less in release) | Low | 1-2 days | Do |
| G | GX shipping trims for GS draws (light form) | GX | ~40-70 ms/s, to be confirmed by step profile | Low | 1-2 days | Do after A |
| B | Hidden-world skip: hold the frame's shipments on the GX thread, prove an opaque cover, release atomically | Render, GPU | ~230 ms/s of render thread (hardware draws 0.77 s per 3 s, minus state apply) | Medium-high | 1-2 weeks | Do if A+G leave under ~10% render headroom |
| C2 | Per-state draw-setup cache keyed on fragment-state content | Render | Only what A leaves; the dump shows same TEV, target and blend, so little | Medium | 3-4 days | Park; revisit if non-texture misses stay above ~2k per 3 s after A |
| D | Dense batched shipment stream (heavy form) | GX, render | ~1-2 us per draw per thread (cache-line traffic) | Medium-high | 1 week+ | Reject for now |
| E | Multi-texture sprite program so runs merge across textures | Render | ~40 ms/s beyond A | High (emitter miscompile history) | High | Reject |
| H | Guest-memory "menu open" flag or guest code patch | All | Largest in theory | High; fade transitions glitch; needs reverse engineering | Unknown | Reject as a proof; at most a hint to arm B |
| I | Render-side lookahead or retroactive cancel of queued world draws | Render | As B but intermittent | Unsound for depth (finding 2) | Medium | Reject |
| J | Command-list prescan on the GX thread | GX, render | Net GX gain ~2 ms/frame after scan cost | Needs a dry run of the list to know state | High | Reject; B gets the render gain without it |

## 4. Phase 0: baseline (no behaviour change)

Use save state slot 1 at the quest counter.

1. **Release:** record 30 s of
   `[mh4u] speed ... game N fps shown N fps gx N ms render ~N ms`.
2. **Diag with `timedraw`:** record, per 3 s:
   - the "repeat misses ... key N; fragment reg changes: ..." line (already printed in
     `gxm_rasterizer.cpp`). This confirms which registers change between sprites. A assumes
     0x82, 0x85 and 0x8E dominate and that 0x83 changes only in filter bits, if at all.
   - `uniform uploads/skips`, `bp_calls`, and the texture, programs and uniforms phases.
3. **Diag with `emuprofgx` and without `noregprof`:** gets the `.ship: take/collect/enqueue`
   and `.collect: regs/luts/uniforms/code` split, and `.reg 25E`. Read
   `gpu thread: queue brake` separately and subtract it from `ship draw (total)` (finding 1).
4. Add two diag counters while here:
   - uploads where `tex_dims_dirty` was the only reason (confirms F0);
   - software-draw key misses split into "binding registers only" and "other" (sizes A
     before it is written).

## 5. Phase 1: texture-only fast path (F0 + A)

### 5.1 F0: uniform dirtiness only when declared

- File: `src/video_core/renderer_gxm/gxm_rasterizer.cpp`, `RasterizerGxm::UploadUniforms`.
- Resolve `const FsUniforms& u = UniformsOf(fs)` before computing `fragment_dirty`.
- Use `(tex_dims_dirty && u.tex_dims)` and `(fb_dims_changed && u.fb_dims)`.
- Leave `tex_dims_dirty` set when it was not consumed, so a program that does declare it
  still gets it.
- Kill switch: `texdimsalways` (diag).

### 5.2 A: data and classification

**Mirror counter.** In `src/video_core/pica/pica_core.h`, next to `fragment_regs_version`,
add `u64 fragment_nonbinding_version`. Bump it only when a changed fragment register is not
a pure texture-binding register. `fragment_regs_version` keeps its meaning (any change), so
`RepeatKey`, the begin-repeat and the vertex memo are untouched.

**Classification.** In `ThreadedRasterizer::ApplyShipment`
(`src/video_core/threaded_rasterizer.cpp`), replace the four-range loop with one
`std::array<u8, 0x300>` class table (vertex-read, fragment, binding). This also trims a
little from every shipment.

Binding registers:
- 0x82, 0x92, 0x9A (dimensions);
- 0x85-0x8A, 0x95, 0x9D (addresses, including the cube faces);
- 0x8E, 0x96, 0x9E (formats);
- 0x83/0x93/0x9B only when `(old ^ new) & 0x70007700 == 0`, i.e. type and wrap bits
  unchanged and only filter bits moved.

Everything else is non-binding and takes the full path: 0x80 (unit enables), 0x81 (border
colour), 0x84 (LOD), all TEV, framebuffer, rasterizer and lighting registers. If Phase 0
shows 0x84 changing between sprites, split it later (bias is a uniform; min/max level is
sampler state); not in v1.

**New `RasterizerGxm` state** (`gxm_rasterizer.h`), captured when a full software draw
finishes in `DrawTriangles`:
- `repeat_nb_version`;
- `repeat_plain`: true when that setup had `!feedback_alpha_only`, `!skip_feedback_draw`
  and unit 0 of type Texture2D;
- `repeat_fb_color_id`: `scene_fb->color_id`.

### 5.3 A: the path

In `RasterizerGxm::DrawTriangles`, after the existing repeat test fails and before
`BeginDraw`.

**Preconditions:**
- `pica.fragment_regs_versioned`, `repeat_ok`, `in_scene`, `vertex_batch.empty()`;
- `draw_epoch == repeat_epoch` and `scene_seq == repeat_scene`;
- no LUT, fog or proctex dirty flag;
- `pica.fragment_nonbinding_version == repeat_nb_version`;
- `repeat_plain`.

**Pass 1: resolve, changing nothing.** For each enabled unit:
- compute the same `memo_key` as `SyncTextureUnits`; require a `tex_memo` hit (key,
  `StructureGen`, surface `modification_tick`);
- require `surface.HasMemory()` and `!surface.HasPendingClear()`;
- require `surface_id != repeat_fb_color_id` (no feedback);
- require the address to be outside the post-effect chain that `BeginDraw` drops.

Any failure falls through to the unchanged full path, which does lookups and uploads and may
end the scene. The fast path itself never reaches the cache, so it cannot break the scene.

**Pass 2: commit.**
1. `FlushDeferredDraws()`: the pending run must go out under the old textures.
2. `BindFragmentTexture` per unit, white for disabled units, as the full path does.
3. Update `bound_surfaces`, set `gpu_serial = runtime.CurrentEpoch()` and `last_use_tick`.
4. Update `tex_dims` and its dirty flag (free for emitted programs with F0).
5. Issue this draw's `sceGxmSetVertexStream` and `sceGxmDraw` over `pending_ring_ranges`
   and update `ring_scene_pos`, as the full path's tail does.
6. Set `repeat_key = RepeatKey()`, so the next draw with the same texture joins the
   existing deferred run.

**Skipped, and why it is safe:**
- `SyncDrawUniforms`: no non-binding register changed value. Dirty bits stay pending for
  the next full draw, as the existing repeat path leaves them.
- Target key and helper: same registers, scene and epoch.
- `SyncAndUploadLUTs`, proctex and fog strips: guarded by the dirty flags.
- `BeginScene` and `SyncState`: unchanged inputs.
- `BindPrograms`: `FSConfig` unchanged by finding 4; `fixed_layout` and blend unchanged.
- `UploadUniforms`: same pipeline and scene, `fs_data` unchanged.
- `ReleaseDraw`: the previous draw already invalidated the same rectangle, which the
  existing repeat path relies on.

### 5.4 Switches, checks, gain

- **Kill switches:** `notexrepeat` (diag flag) and the file `ux0:data/mh4u/notexrepeat` for
  release, read once like `nopreshade`.
- **Check mode `texrepeatcheck` (diag):** on every fast-path hit, also build
  `FSConfig{regs}` with the same adjustments as `BindPrograms` and compare its hash with the
  last bound one; compare the target-key hash; re-run the unit loop without the memo and
  compare the resolved `SceGxmTexture`. Log mismatches like `vsmemocheck`; expect zero.
- **Stats:** add "texture-only" to the repeat-miss line.
- **Expected:** the ~15k key misses per 3 s drop from ~45 us to ~8-10 us (diag), so
  software draws go from ~1.4 s to ~0.9 s per 3 s. Release estimate: game 24-26 to ~27-28
  fps, with a smaller brake on the GX thread.
- **Risks:**
  - a register wrongly classified as binding (mitigated by the check mode and the
    conservative list);
  - the sprite sequence alternating a non-binding register too, so the path rarely fires
    (Phase 0 step 4 shows this before any code is written).

## 6. Phase 3 design: hidden-world skip (B)

Presented before Phase 2 because it needs the most review. Implement after Phase 2 unless
Phase 1 results say otherwise.

### 6.1 Discovery first (diag only, flag `coverdiag`)

Log per command list and per frame at the quest counter, item box and smithy:
- the index and kind of every draw into the main target;
- for the first 16 GS draws after the last hardware draw: screen rectangle, blend,
  alpha-test and depth state, TEV raw words, texture address, format and size, vertex
  alpha;
- whether the world and the cover are in the same list;
- chained-list count (`g_pica_probe[4]`);
- memory fills covering the main colour and depth buffers, and their position relative to
  the first depth-tested draw;
- any draw between the world and the cover that samples the main target or draws
  elsewhere.

**Gate.** Proceed only if:
- one rectangle, or a few abutting ones, cover the whole 240x400 target;
- the texture and TEV conditions in 6.3 can hold;
- H1 (6.6) holds in every scene visited.

### 6.2 Mechanism

**Hold.** Lives in `ThreadedRasterizer`, on the GX thread.
- While armed, `ShipDraw` and `DrawTriangles` build shipments as now, including
  `QueuePreshade`, so the core-3 worker shades them during the hold. They push the op into a
  local `held` vector instead of enqueuing it.
- The hold starts at the first command list of a frame (hook in the `GxPush` lambda of
  `GPU::QueueCmdList`, `src/video_core/gpu.cpp`).
- It ends at the first of: the next GX task that is not a list; the GX queue going idle
  (before `gx.work_wake.Wait()` in `GxLoop`); 1024 held shipments; any abort.
- Aborts (release everything unmodified, in order):
  - any non-shipment enqueue from this thread (fill, transfer, invalidate, flush,
    interrupt-after-queue);
  - any blocking wait on the render thread: `RingWaitForSpace`, the emergency-budget wait
    in `BeginShippedDraw`, `WaitGpuIdle`, `RunSync`. Without this the GX thread deadlocks
    on a render thread that has nothing queued.

**Per-shipment summary**, filled at ship time from `emu_pica` registers, only while holding:
colour and depth address, framebuffer size; `reads_depth` (depth test with a function other
than Always, or stencil test); the address ranges of enabled texture units.

**Cover rectangle.** Computed by the pre-shade worker (`PreshadeLoop`) right after shading a
GS draw, from its own mirror and the 6 output vertices. Stored in the shipment as
`cover {rect, texture infos, uv bbox, valid}`. Logic in a new pure file,
`src/video_core/cover_proof.{h,cpp}`, testable on the host build.

**Decision.** At hold end, on the GX thread:
1. Wait up to 2 ms for candidate shipments to reach `ShadeDone`; otherwise no skip.
2. Take the union of valid cover rectangles into target T and find the first index where it
   covers the whole framebuffer.
3. Check the obligations in 6.4.
4. If they pass: flag every draw into T before the first cover piece as droppable, prepend a
   commit op, and push the batch with a new `GpuThread::EnqueueBulk` under one lock (no
   interleaving by other producers). The bulk waits for depth below `MaxDepth / 2`, then
   pushes all.

**Commit op** (render thread, head of the batch): for each cover piece, resolve its textures
with `res_cache.GetTextureSurface`; require a proven-opaque verdict (6.3); record
`(surface id, modification_tick)`. Only draw shipments run between the commit and the cover,
so no invalidation can change those surfaces in between; `TrimSurfaceCache` runs only in
`EndFrame`.

**Droppable shipment op:** run `ApplyShipment` as usual; skip the draw; for an in-ring
hardware payload, call a new `RasterizerGxm::DropShippedDraw` that sets `hw_ring_end` and
calls the existing `DropDraw` so ring space is retired. At each cover draw, assert the bound
surface matches the recorded one; a mismatch is logged as an error and counted (it cannot
happen by construction).

**Arming.**
- The worker runs the cheap geometry and state test on top-target GS draws at all times
  (state verdict memoized per fragment version; ~6 vertex checks per draw).
- It publishes `cover_seen_list_seq`.
- The GX thread holds only when one of the last two lists had a cover, and disarms after
  two held lists without a drop.
- The prediction decides only whether to pay the hold's latency, never whether to drop.

### 6.3 What counts as an opaque piece

**Geometry.** Two triangles forming an axis-aligned rectangle; equal positive w; z inside
the clip range. Interior edges shrink by one pixel; edges at or beyond the target boundary
count as the boundary.

**State.**
- colour mask covers every channel the target format has;
- depth test off or Always; stencil off;
- alpha test passes for alpha 255;
- scissor off, or Include containing the target;
- fragment mode Default;
- blend on with Add, Subtract or ReverseSubtract and destination factors Zero or
  OneMinusSourceAlpha; or blend off with logic op Copy.

**Alpha equals 255.** A small abstract evaluation of the TEV alpha chain over {One, Unknown}:
vertex alpha is One if all corners are >= 1.0; a texture is One if proven opaque; a constant
is One if its alpha byte is 255; Replace and Modulate propagate One; anything else is
Unknown.

**Texture opacity.** A verdict per surface, keyed on `upload_hash`
(`src/video_core/rasterizer_cache/surface_base.h`). Computed lazily in the commit op by
scanning the guest source bytes, accepted only if their hash equals `upload_hash`, so it is
exact for what was uploaded.
- Formats: ETC1A4, RGBA4, RGBA8, RGB5A1, IA4, IA8, A4, A8 are scanned; formats without
  alpha are opaque.
- Whole texture, UVs inside [0,1], single mip level in v1.
- If discovery shows transparent padding, add an 8x8-tile bitmap.

### 6.4 Obligations checked over the whole held batch

1. No kept draw before the union completes samples T or D, or draws to another target while
   sampling them.
2. No kept draw anywhere reads depth or stencil on D (exact: the hold spans to the end of
   the list).
3. All dropped draws share T's framebuffer registers.
4. Cover textures lie outside VRAM and outside every target range in the batch.
5. T and D were filled since the last present (the evidence for H1).

### 6.5 Switches, tests, gain, risks

- **Kill switches:** `nocoverskip` (diag flag) and `ux0:data/mh4u/nocoverskip` (release).
- **`coverhold` (diag):** hold and decide but never drop; isolates the latency effect.
- **`covermagenta` (diag):** replace the dropped world by a magenta clear of T at the
  commit. Any magenta pixel in a screenshot is a hole in the proof. Run it through opening,
  closing and scrolling each menu, including fades.
- **Gain:** render-thread hardware draws ~0.77 s per 3 s to ~0.08 s; GPU fragment time for
  the village gone. The GX thread is unchanged and becomes the limit at ~25-28 ms per frame.
- **Risks:**
  - **Heap.** Held shipments can grow the pool. `Shipment` is ~4 KB, mostly two inline
    `Pica::Uniforms`. Measure with `-DVITA_ALLOC_TRACE=ON` and `live.txt`. If the peak grows
    by more than ~1 MB, first shrink `SetupSync` to a small inline row window with an
    overflow pointer, which also lowers today's pool.
  - **Frameskip signals.** `EstimatedBacklogUs` sees a bulk of ~700 ops; at ~30 us each
    that is ~21 ms, under the 120 ms threshold. Re-check on the village walk
    (`NOSHOT=1 walk.sh`).
  - **Latency.** While armed, the render thread starts a frame only after its list is
    parsed.

### 6.6 Assumption H1

Dropping the world also drops its depth writes.
- Within the frame this is checked exactly (obligation 2).
- Across frames it is safe only if MH4U overwrites the main depth buffer before the first
  depth-reading draw of every frame. This cannot be proven inside one frame.
- The hold therefore arms only when the current frame's fills of T and D were seen.
- The GX thread tracks a "depth poisoned" flag per dropped batch, cleared by a covering
  fill. A depth-reading draw or a transfer sourcing D while the flag is set logs an error
  and disables the feature for the session.
- Discovery must show zero such events across village, hunt and all menus before B ships.

**Open decision:** whether H1, verified this way, is acceptable under the "no one-frame
glitches" rule. It is the only non-exact element in the plan.

## 7. Phase 2: GX thread trims (G)

Pick from the Phase 0 step profile. All exist in the code as described; none has a measured
size yet.

1. **Shared bulk enqueue.** `ShipDraw` appends ops to a 16-entry local array, pushed under
   one lock via `EnqueueBulk`. Flush at the end of each GX task and before any other enqueue
   or wait from this thread. Saves a lock, deque push and wake test per draw; B needs the
   same API. Kill switch `noshipbatch`.
2. **Build the payload in place.** The inline software path in `PicaCore::TryShipDraw`
   (`src/video_core/pica/pica_core.cpp`) zero-initialises a ~360-byte `DrawPayload` on the
   stack, then `ShipDraw` copies it. Add a rasterizer call that hands out the shipment's
   payload to fill directly.
3. **Cache the last target in `NoteShippedDraw`.** It runs four linear scans per draw
   (`IsRepeatTarget`, `RecordDrawTarget`, two `RecordTopTarget`). Remember the last colour
   and depth address and skip when unchanged, as `MarkGLWritten` already does.
4. **Redundant 0x25E writes.** If the step profile shows `.reg 25E` above ~10 ms/s, check
   `PrimitiveAssembler::Reconfigure` semantics and skip the event when the topology is
   unchanged and the assembler is empty. Not yet verified safe; read that function first.
5. **Uniform window.** `WriteUniformFloatRegRange` sets `uniforms_sync_dirty` even for
   identical values. Narrowing the shipped window to rows that changed is possible but
   small; last.

No check mode is needed for 1-3 beyond the existing picture tests, since they do not change
what is shipped. Target: 40-70 ms/s at the quest counter, village walk unchanged.

## 8. Verification after each phase

- **Speed:** slot 1, release, 30 s of the `[mh4u] speed` line (game fps, shown fps, `gx` ms,
  `render` ms). Then diag `timedraw` and `emuprofgx` for the split.
- **Picture:** quest counter, quest board pages, item box, smithy with an armour preview,
  and opening and closing each (fades). Regression scenes: ship deck in both views, ship
  cutscene, village walk, keyboard screen, character select.
- **Check modes at zero mismatches** before a change is left on by default:
  `texrepeatcheck`, `covermagenta`, and the cover assert counter.
- **Heap:** alloc tracer live table before and after Phase 3.
- **Each phase lands as its own commit**, default-on with its kill switch, so a regression
  can be bisected with a file on the memory card.

## 9. Expected trajectory (estimates)

| After | Render thread | GX thread | Game fps at the counter |
|---|---|---|---|
| Today | ~95% busy | ~870-900 ms/s incl. ~25% brake | 24-26 |
| Phase 1 | ~80-85% at 25 fps | brake shrinks | ~27-28 |
| Phase 2 | same | -40-70 ms/s | ~28-30, no margin |
| Phase 3 | ~55-60% at 30 fps | the limit, ~25-28 ms per frame | 30 (capped) |

## Critical files

- `src/video_core/renderer_gxm/gxm_rasterizer.cpp`
- `src/video_core/threaded_rasterizer.cpp`
- `src/video_core/gpu_thread.cpp`
- `src/video_core/pica/pica_core.cpp`
- `src/video_core/gpu.cpp`
