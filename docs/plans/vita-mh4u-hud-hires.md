# Plan: HUD at screen resolution (`hudres`)

Planned 2026-11-19 (Fable), from the code at 5a0b34b24; nothing run on the console yet. Goal: the
game's whole 2D layer (HUD, text, icons, menus) drawn at the Vita's resolution over the stretched
3D picture, as a pause-menu option. Replaces the removed Sharp HUD text (58c3ea0d7..f834922b7,
reverted in 57d1fd733), which replayed only the text afterwards: text overlapped by later 2D draws
fell back to native, so menus like the Quest Counter mixed sharp and soft text, and only the
software geometry-shader path was covered.

## 1. Verdict

Workable. Every draw path goes through `RasterizerGxm::BeginDraw` (`DrawTriangles` for software
and software-GS draws, `AccelerateShippedDraw`, `AccelerateGsDraw` for GPU GS); the fast paths
(software repeat, `TexOnlyRepeat`, begin-repeat, deferred runs) only fire inside the scene the last
full draw opened, so they follow a redirect by themselves. The present already normalises by the
texture size.

- GPU estimate +2 to +5 ms a frame in play (section 9): ~30 -> 26-28 fps in GPU-bound fields, so
  opt-in; `hudres=2` (1.5x) is the cheaper variant.
- Three facts to measure first (Stage 0): post-grade draws with depth or stencil; scenes the 2D
  layer is split into; screens with no grade pass.
- Frames without a grade pass stay at 400x240 in the first version (soft but consistent).

## 2. What the code does today

- **Split marker:** `hud_phase` is set in `BeginDraw` on the draw `SyncTextureUnits` recognised as
  the grade (`IsGradeDouble`: a feedback read of its own target with the doubling combiner);
  `hud_target` is its colour buffer. "Post" = `hud_phase && cb == hud_target`. Cleared at every
  display transfer and at `EndFrame`; needs the defaults (not `keepfeedback`/`nogradeblend`).
  `EndFrame` runs at every VBlank swap (60/s against the game's 30), so a swap may land inside
  the 2D layer: the redirect state must not depend on it.
- **MH4U frame order:** fill -> bottom screen into the same main colour buffer (240x320
  viewport) -> bottom transfer -> world -> grade -> top 2D -> top transfer. The bottom screen is
  never "post". Movie frames also run the grade (empty 2D layer).
- **Target:** main target 256x512 RGBA8; take the 240x400 rectangle from the grade draw's viewport,
  never hard-coded. The hi-res target needs two scale factors: 544/240 along framebuffer x (the
  screen's vertical), 960/400 along y in Wide (906/400 in 5:3).
- **G2 slots:** `DisplaySlot` x4 filled by `SmoothDisplayTransfer` through `GpuBlit`, returned by
  `AccelerateDisplay`. `GpuBlit` point-samples and runs its own scene unless the open scene's
  colour is the same cached surface. The blit program copies all four channels.
- **Present:** `DrawGuestScreen` magnifies linearly, rotation in the texture coordinates; the
  sharp program is picked from the option alone, even at 1:1; `FxaaPass` refuses strided textures.
- `VideoOptions::msaa` is saved but unused by the renderer. CDRAM budget 96 MiB.

## 3. Design

### 3.1 State machine (render thread)

| State | Meaning | Leaves by |
|---|---|---|
| Idle | Normal path | Grade on a colour target, option on, no blocker in the last 2 frames -> Armed |
| Armed | Grade seen, no 2D draw yet | First post draw -> Active (`HudBegin`); display transfer or fill of the target -> Idle (no copy: cutscenes and movies cost nothing) |
| Active | Post draws go to the hi-res target | Matching top transfer -> Shown; blocker -> `HudBail` -> Idle; fill over the target -> abandon -> Idle |
| Shown | The transfer paired the hi-res picture with a display buffer; the low-res target lacks the 2D layer | Fill covering the target, or the next grade -> Idle; any read of the low-res target first -> `HudResolveDown` |

Armed stores `hud_target`, the guest colour `SurfaceId` and the source rectangle `R` (the grade
draw's viewport). Active and Shown are not cleared by `EndFrame` or unrelated transfers. On arming,
`repeat_ok = begin_repeat_ok = false` and bump `draw_epoch`/`nondraw_epoch`.

### 3.2 Hi-res targets

- Size from the layout's top rectangle (`GetFramebufferLayout().top_screen`): surface width =
  rectangle height (544), height = rectangle width (960 Wide, 906 in 5:3). `hudres=2`: 360x600.
- Pool of up to 4 RGBA8 surfaces outside the cache (2.09 MiB each, ~6.3 MiB in steady state). A
  `Surface` constructor argument for an exact storage scale (colour surfaces are multiplied by the
  render scale today).
- Work surface at `HudBegin`: one no valid slot references and not `presented`; else the slot
  transferred longest ago (invalidate it). Never the two most recently transferred.
- One `Framebuffer` for all of them: `Framebuffer::Retarget(Surface*)`; its `color_id`/`depth_id`
  are null (every `GetSurface(fb->color_id)` is already guarded).

### 3.3 `HudBegin`: the upscale copy inside the hi-res scene

At the first post draw, before `BeginScene`: pick the work surface and retarget; `EndScene()` (the
world scene ends here anyway); `BeginScene(&hud_fb)`; draw the copy quad first (as
`DrawPendingColourClears`: blit ring quad, blit shaders, no blending, all channels; source = guest
main target, linear, clamp; coordinates = `R` normalised); set serials on both surfaces; invalidate
the pipeline binding, `state_cache`, `uniform_pipeline`. Not `GpuBlit`: that would cost a scene of
its own and one more load/store of 522,000 pixels. To measure: whether GXM loads the target at the
scene start although the quad covers it (~0.5-1 ms if so).

### 3.4 Redirecting draws (`BeginDraw`)

- `redirect = hud_state == Active && cb == hud_target`; the first such draw calls `HudBegin`.
- `SyncTextureUnits(fb)` keeps the guest framebuffer (feedback detection unchanged).
- `BeginScene(redirect ? &hud_fb : fb)`; `SyncState(*setup.helper, redirect ? &hud_xform : nullptr)`.
- The guest `FramebufferHelper` stays (it invalidates the drawn region of the guest surface).
- `SyncState` transform `{kx, ky, R, target size}`: viewport `vx = (vp.x - R.left) * kx`,
  `vy = (vp.y - R.bottom + 0.5) * ky` (the half-pixel shift is half a guest pixel), sizes scaled;
  region clip and scissor uniforms mapped with floor/ceil and clamped; `scissor_covers_draw` from
  the mapped values. `fb_dims` follows `scene_fb`. No depth on `hud_fb`: depth ALWAYS, no writes.

| Path | Redirect |
|---|---|
| Software draws, software GS | `BeginDraw` |
| GPU GS (`AccelerateGsDraw`) | `BeginDraw`; guard `PrepassEligible` with "not redirected" |
| Shipped draws | `BeginDraw`; same guard |
| Software repeat, deferred runs | same scene and state; `FlushDeferredDraws` draws into the open (hi-res) scene |
| `TexOnlyRepeat` | same; keep `repeat_fb_color_id` = the guest colour id while Active |
| Begin-repeat | same scene; `RepeatKey` includes the framebuffer registers |
| Fills (`AccelerateFill`) | a fill over the target ends the state; none redirected |
| Draws into other targets mid-layer | normal `BeginScene`; each split costs a hi-res load + store (counted) |
| `GpuBlit` | its merged test needs `scene_fb->color_id` (null for `hud_fb`): ends the scene |

### 3.5 Display transfer and slots

In `AccelerateDisplayTransfer`, before `SmoothDisplayTransfer`: if Active, the source is the hud
guest surface, its rectangle equals `R` and the output is 240x400 unscaled: `FlushDeferredDraws`,
`EndScene`, point the output address's `DisplaySlot` at the work surface (slot gains a pool index
and `flip` = `(source.is_tiled != dest.is_tiled) != config.flip_vertically`), state Shown, return
true (no blit: saves the ~0.8 ms transfer). Active but not matching: `HudBail`, normal path. Shown
and the same frame transferred again elsewhere: that slot points at the same pool surface. Idle or
Armed with the option on: G2 path regardless of `smooth` (no flicker between 5:6:5 and 8-bit).
`AccelerateDisplay`: condition `smooth || hudres`; a hi-res slot returns the whole pool surface and
the flip.

### 3.6 Present

`GuestFramebuffer` / `GuestScreen` gain `tex_flip`, `tex_exact` (the texture is the picture at
display resolution) and `tex_bpp` (real bytes per texel; also fixes `dumpframe` for G2's RGBA8
slots). `DrawGuestScreen`: `tex_flip` swaps v0/v1; `tex_exact` at 1:1 uses point filtering and the
plain program, never the sharp one. `BeginFrame` skips `FxaaPass` for a `tex_exact` screen.

### 3.7 Keeping the guest-visible picture correct

The low-res main target never gets the 2D layer while the option works. Readers:

| Reader | Handling |
|---|---|
| The top display transfer | ours (3.5) |
| A draw into another target sampling the main target (menu background capture, transitions) | after `SyncTextureUnits`: Active -> `HudBail`, Shown -> `HudResolveDown` |
| `AccelerateTextureCopy` reading the target | same |
| `FlushRegion` / `FlushAll` (CPU read-back, state save) | same |
| The next frame drawing over the target without a colour fill | Shown and a non-post draw into `hud_target` with no fill since the transfer -> `HudResolveDown` first (Stage 0 checks the fill) |

`HudResolveDown` / `HudBail` = `EndScene` + `GpuBlit(hi-res -> guest, R)` with a new linear option.
After a bail the frame continues on the normal path (soft that frame, complete and in order).

Blockers (cannot be done at hi-res): depth or stencil needed, shadow pass, no colour. Evaluated also
while Idle: the redirect only starts when the last 2 frames had none, so a screen whose 2D layer
always needs depth (an equipment preview) simply stays native, without a bail every frame.

State save: `FlushAll` resolves first. Load: `ClearAll` resets to Idle and invalidates slots.

### 3.8 Unchanged (checked)

DstAlpha blending (minimap masks, draw #66): the hi-res target has its own alpha and the copy
carries the graded frame's alpha. Alpha-only feedback draws: as today, on the hi-res target. Render
scale: the copy samples by normalised coordinates. Bottom screen: before the grade, never
redirected. No grade: stays Idle. 2D before 3D: part of the low-res frame, upscaled by the copy.
Scenes per frame: same count (the hi-res scene replaces the transfer's blit scene) unless split.

## 4. Stage 0: measure first

Counters: `post_depth` (post draws with depth or stencil), `post_scenes` (scenes begun on
`hud_target` while `hud_phase`), `hud_cut` (`EndFrame` with `hud_phase` set), grades against top
transfers. Slots 1-7, title, start menu, equipment preview, guild card, map, loading screen, chat
keyboard.

| Question | Decides |
|---|---|
| Post draws with depth/stencil? Where? | Hi-res depth (11.1) needed in v1? |
| `post_scenes` per frame (camp, village, quest counter) | Cost; above ~2, find the splitter first |
| `hud_cut` > 0? | Confirms the state must outlive `EndFrame` |
| Screens with no grade | Scope of 11.2 |
| Main target filled every frame? Transfer flip? Source rectangle? | 3.7 last row; `flip`; `R` |
| What are the two full-screen 2D quads? | Dither/grid risk |

## 5. Implementation steps

1. Option `hudres` (0 off, 1 screen, 2 = 1.5x) in `gxm_video_options`, pause menu choice "HUD
   resolution" after Smooth gradients; kill switch `ux0:data/mh4u/nohudres`.
2. `Surface` exact-scale constructor argument; `Framebuffer::Retarget`.
3. Rasterizer state: `HudState`, `hud_color_id`, `hud_rect`, `HudXform`, pool, `hud_fb`,
   `DisplaySlot` hud index + flip, blocker history, counters.
4. `HudBegin`, `HudBail`, `HudResolveDown`, `HudAbandon`, `HudEnsurePool`; `GpuBlit` linear option.
5. `BeginDraw`: arm at the grade, blockers, reader check, redirect of `BeginScene`/`SyncState`;
   serial for the work surface; scene trace marker.
6. `SyncState` transform; `PrepassEligible` guard; `repeat_fb_color_id`.
7. Transfer/slots: `AccelerateDisplayTransfer`, `AccelerateDisplay`, `AccelerateFill`,
   `AccelerateTextureCopy`, `FlushRegion`/`FlushAll`, `ClearAll`.
8. Present: `guest_framebuffer.h`, `renderer_gxm.cpp`, `gxm_present.*`, `emu_window_vita.cpp`.
9. Diagnostics, tests. 10. Docs.

## 6. Settings

`video.txt` `hudres=0|1|2`, default 0, read live. Pause menu "HUD resolution": "3DS (400x240)" /
"Screen" / "Screen, lighter (1.5x)". It turns on the 8-bit display copy by itself. The present
skips FXAA and the Sharp filter for hi-res frames, so HudBegin's copy does them instead: the
sharp-bilinear program on the copy quad, and FXAA of the 3D picture at its own resolution into a
scratch surface (its own scene) ahead of the copy. Diag `hudcopyplain` keeps the plain copy.

## 7. Diagnostics

Release `cpu` line `hud r s b` (redirected draws, hi-res scenes, bails per second); `hudreslog`
(transitions, bails by reason, pool use, `R`, flip); `dbghudrestint` (magenta instead of the copy);
`dbghudresboth` (A/B); `hudresnocopy`, `hudresnoresolve`; `hudreslinear`; `hudresshift<N>`;
`scenetime` marker; existing `hudlog`, `dbgnohud`, `dbgonlyhud`, `logxfer`, `presenttime`.

## 8. GXM scenes

Today: world scene(s) (2D merged into the last) -> transfer blit scene -> present. With `hudres`:
world scene(s) -> hi-res scene (copy + 2D) -> present: same count. The hi-res scene stores 522,000
pixels (maybe loads them); the transfer blit's 96,000 store is gone. Each interruption of the 2D
layer costs one more hi-res load and store.

## 9. Cost estimate (to confirm)

| Item | GPU ms |
|---|---|
| Copy quad (522,000 textured pixels) | +1.0 to 1.5 |
| Target load at scene start, if any | +0 to 1.0 |
| Two full-screen 2D quads at 5.4x pixels | +0.9 |
| Sprites and alpha-only draws, fill part at 5.4x | +0.5 to 2 |
| Transfer blit gone | -0.8 |
| Present 1:1 point (sharp filter no longer run if it was on) | 0 (or -1.9) |
| **Net** | **about +2 to +5** |

`hudres=2`: roughly +1 to +2.5. CPU: render thread +0.1-0.2 ms a frame. Memory ~6.3 MiB CDRAM.

## 10. Risks

1. Too slow where the GPU is the limit: opt-in, `hudres=2`, option text says so.
2. 2D layer split into several scenes: Stage 0 gate.
3. Post-grade draws with depth: blocker rule (that screen stays native); 11.1 later.
4. A swap mid-layer: the state survives `EndFrame`; never reuse a surface the guest may show.
5. A dither/grid pattern in the 2D layer magnified 2.4x: clear the dither bits while on.
6. Readers of the main target after the frame: Shown + `HudResolveDown`; test start menu, Gallery,
   quest-complete and loading transitions.
7. 2D/3D registration (name tags, lock-on): scaled half-pixel term, `hudresshift`.
8. Nearest-filtered fonts blocky at 2.4x: `hudreslinear`.
9. Frames without a grade stay soft (11.2).
10. Frameskip drops a transfer while Active: next fill abandons, next grade restarts.
11. `dumpframe` bytes per pixel for RGBA8 slots: fixed by `tex_bpp`.
12. CDRAM in long sessions: allocated once; watch the health line.

## 11. Follow-ups

1. Hi-res depth for 2D draws starting from a cleared depth (544x960 D24S8, 2.1 MiB).
2. 2D-only frames (no grade): redirect from the first top draw, the clear as a quad.
3. FXAA or the sharp filter inside the copy (3D only, HUD untouched).
4. Skip the resolve when the next frame provably overdraws everything.

## 12. Test plan

Lossless pictures (surface dump / `dumpframe` after the `tex_bpp` fix), three captures per case:
off, `hudres=1`, `dbghudrestint`.

| Slot | Check |
|---|---|
| 1 camp | HUD text and gauges sharp, "(No Item)"; `hud r` ~ the 99 post draws; fps (no margin) |
| 2 heavy field | fps and threads (pre-shade and render thread are the limit) |
| 3 water | 3D part unchanged |
| 4 village | minimap mask correct; name tags registered with the 3D |
| 5 item box | open over the pouch: **all** text sharp and covered correctly |
| 6 Gallery | cutscene: no copy (`hud s0`), fades correct; menu sharp |
| 7 quest counter | every text sharp, none soft; fps (GX-bound) |

Also: title and loading screens, equipment preview (blocker path), chat keyboard, start menu in a
hunt, Wide and 5:3, two screens and back, render scales, FXAA and sharp on, `smooth` on/off,
toggling the option repeatedly, save/load slot 8, PS button, 20 minutes watching `hud b` and the
health line. Pass: no wrong pixel against off except resolution; slots 5 and 7 fully consistent;
cost within the estimate.
