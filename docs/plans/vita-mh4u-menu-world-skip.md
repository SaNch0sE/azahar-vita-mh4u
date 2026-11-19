# MH4U: skip the 3D world under a full-screen opaque menu

Plan 2026-11-19 (Fable), read-only research; nothing built or run yet. Code addresses read from
the two mod builds (USA v1.1 local build, USA v1.0 web build); EUR from MH4UltimateDC's
`eur_v110.txt`, untested. Measured motivation: clearing the world units' view-0 bit at the quest
counter saved ~29 ms GPU per frame with an identical picture (`worldoff`, performance notes).

## 1. Verdict

- **Hook point:** a trap in `sMhMain::move` on the instruction just before the call to
  `sLtRender::move`: all updates of the frame are done there, and the unit draws run
  synchronously inside that call.
- **Detection:** the render thread proves the cover from the real 2D draws of an earlier frame;
  the trap hashes the game's large opaque panes (the "key") and arms only while the key equals
  the key of a proven frame. A pure game-data cover test cannot be exact with the mod: vertex
  slots and null matrices are local and written during the draw, the mod shifts groups and picks
  their view at draw time, and the 16:9 projection is patched in code.
- **Skip:** clear bit 16 of `unit+0x04` on every non-layout unit, with an exact restore list.
- **Timing:** any change to a cover pane changes the key at that frame's trap, so the world is
  back before that frame is built. A data-only tick cannot guarantee this (a fade would show a
  brightness error for a frame, an instant close the clear colour for 33-66 ms) - it serves only
  as a watchdog.
- **Open risks:** patching code after a state load (test slots hold unpatched code); units that
  keep state in their draw (not auditable statically; tested on the console).

## 2. Disassembly findings

Main loop (`sMhMain::move`, v1.1 `0x00af5250`, one straight function): singleton updates through
vtable slot `+0x1C` (`sUnit::move` early, `0x00af5398`); the mod's hook at `0x00af550c`; then
`0x00af5510 ldr r1,[r0]`, `0x00af5514 ldr r1,[r1,#0x1C]`, `0x00af5518 blx r1` = `sLtRender::move`
(`0x00bfa670`) -> `buildFrame` (`0x00bf5a4c`) -> `sMhScene::draw` (`0x00aee5d4`) synchronously,
8 scene slots, `sUnit::draw` at `0x00aee984` (and a shadow pass at `0x00b6efe4`); then a few
post-render calls and three more updates.

`sUnit::draw` (v1.1 `0x006450cc`) walks `[sUnit+0x614]` lines (use this count, clamped to 64; the
experiment assumed 32). A unit's `draw(cDraw)` (vtable `+0x30`) is called only when: state
`(flags & 7) == 2` (`0x00645160`); flag bits 11 and 14 (`0x0064516c`); `unit+0x08 &
(cDraw+0x1BC >> 16)` (`0x00645178`); bit `16 + view` of `unit+0x04` (`0x00645184`);
`unit+0x18/+0x1C` within the mask at `sUnit+0xD28/+0xD2C` (`0x006451a0`). Nothing is written back
per unit; clearing the view bit skips only the unit's own draw. Updates run separately
(`0x006451f4`).

Texture chain (medium confidence, logs only): `sprite+0x04` texture word (bit 31 font page; else
low 24 bits index `group+0x24`); entry `rTexture*`; `[rTexture+0xD0]` texture object: `+0x18`
image address, `+0x1C` width (bits 0-15) / height (16-28), `+0x21` mips, `+0x24` format (11 ETC1,
12 ETC1A4), `+0x28` GL format/type (opaque: high half `0x6754` RGB, `0x6757` L, `0x675A` ETC1).

## 3. Addresses

| Item | USA v1.1 | USA v1.0 | EUR v1.1 (untested) |
|---|---|---|---|
| `sMhMain::move` | `0x00af5250` | `0x00af4bd8` | `0x00af52a0` |
| Mod hook (`bl`) | `0x00af550c` | `0x00af4e94` | `0x00af555c` |
| **Trap site** (`ldr r1,[r0]`, `0xE5901000`) | `0x00af5510` | `0x00af4e98` | `0x00af5560` |
| `sLtRender::move` | `0x00bfa670` | `0x00bf9ff8` | `0x00bfa6c0` |
| `buildFrame` | `0x00bf5a4c` | `0x00bf53d4` | `0x00bf5a9c` |
| `sMhScene::draw` | `0x00aee5d4` | `0x00aedf5c` | `0x00aee624` |
| `sUnit::draw` | `0x006450cc` | `0x00644b24` | `0x006450f4` |
| View-bit test | `0x00645184` | `0x00644bdc` | `0x006451ac` |
| `uLayout::draw` | `0x00b04774` | `0x00b040fc` | `0x00b047c4` |
| Per-group draw call | `0x00b047e0` | `0x00b04168` | `0x00b04830` |
| `sUnit*` | `0x010572F8` | `0x010572F0` | as v1.1 |
| `sMhLayout*` | `0x01057534` | `0x0105752C` | as v1.1 |

Words to verify around the trap (v1.1; same at the other sites): `0xaf5500 E5901000`, `0xaf5504
E591101C`, `0xaf5508 E12FFF31`, `0xaf550c E5940290` or any `bl`, `0xaf5510 E5901000`, `0xaf5514
E591101C`, `0xaf5518 E12FFF31`, `0xaf551c E59402B8`.

Layout family: a unit whose vtable slot `+0x30` is `uLayout::draw`, or `uLayoutMonnyan`
(`0x00E60E14` v1.1, 0x30 lower in v1.0). Eight vtables share `uLayout::draw` (v1.1 `0x00E5EF3C`,
`E61D68`, `E62AC0`, `E63520`, `E635EC`, `E6415C`, `E6463C`, `E6BB40`); the `worldoff` experiment
skipped only the main one (it would hide the software keyboard).

## 4. Design

**Trap:** replace the trap-site word with `svc 0xF1` after verifying the eight words (accept an
already patched word); the handler emulates `r1 = [r0]` and runs the decision, whatever the kill
switch. Patch at boot like the movie-poll patch; after a state load with unpatched code wait 120
guest frames, then patch in the `RunSlice` prologue; never rewrite an already patched word.
Fallback: point the `sLtRender` vtable slot (`0x00E6CAD8`) at an unmapped address and handle the
abort (data-only, restorable; unproven).

**Cover proof (render thread):** for each 2D sprite draw into `hud_target` in `hud_phase`, opaque
= vertex alphas 1 and standard blend and final combiner alpha provably 1, depth/stencil off, full
colour mask; mark a 16-px cell grid over 240x400 for axis-aligned quads with both sides >= 40 px
(shrunk by 1 px); once full, count later depth or non-sprite draws ("late 3D"); publish
`{tag, covered, late3d}` at the frame's end. Tag: the trap's sequence number, captured when the
frame's display transfer is queued and carried through the GX and render queues. Only alpha-less
formats count at first (the ETC1 planks of `ui409_boad_haikei`); ETC1A4 needs a per-tile opacity
bitmap made at decode time (later).

**Key (trap):** walk every layout-family unit's draw list (`unit+0x120`, next `group+0x48`),
shown groups (`0x400`), any view; skip groups with alpha (`+0x3F`) below 255 and subtrees of
invisible or non-opaque nulls; candidates: visible sprites with four corner alphas 255, rotation
0, scaled size >= 24x24, plus visible window panes; hash the sprite bytes, ancestor null words,
group order/position/colour/flags/texture array, unit flags - masking volatile words (`group+0x40`,
flag `0x200`, dirty byte, slot bits). Plausibility gate: candidates' clipped area >= one screen
(positions add, scales multiply, screen point (200 - x, 120 - y)). ~0.1-0.2 ms a frame.

**Decision and restore:** arm when the key is non-zero and not blacklisted, the newest proof is
covered with no late 3D, its tag is at or after the frame this key value started, and it is under
30 frames old; disarm at the first trap where any fails (before the build, so that frame has the
world). While armed, each trap clears bit 16 on every non-layout unit that has it, recording
`(unit, vtable)`; restore by walking live lines and setting the bit on listed live units with the
same vtable. Mispredict (a proof says "not covered" for an armed tag): disarm, blacklist the key,
log. Watchdog in `Tick`: a presented frame with no trap while armed restores at once. Possible
upgrade: hide the world through `unit+0x08` or the `sUnit+0xD28` mask instead of per-unit bits.

**Save states:** `BeforeSave` restores all bits; on a process change drop the list without
writing and wait 60 frames; states made with this build need it (`svc 0xF1`). A state saved under
the old `worldoff` experiment has its bits cleared for good: do not reuse such states.

**Bookkeeping:** the draw loop writes no per-unit state (confirmed); individual `draw` methods
(joint matrices, LOD, "was drawn" flags) not audited; the shadow pass uses `sUnit::draw` with its
own view; menus with a 3D model on top (equipment, smithy, guild card) are blocked by the late-3D
rule until measured.

## 5. Switches and diagnostics

`nocoverskip` (off, trap still runs), `nocovertrap` (no patch), `covershadow` (decide and count,
never write), `coverlog` (candidates and hidden unit classes once a second), `covermagenta` (diag:
magenta fill before the 2D layer on armed frames - any magenta pixel is a hole). Release cpu line
`cov a t k m u` (armed frames, arm transitions, key changes, mispredicts, units hidden); one log
line per transition with its reason.

## 6. Order

| Step | Build | Check on the console |
|---|---|---|
| 1 | Trap patch, empty handler; remove `WorldOff()` | slots 1-7 three times: no power-off or slowdown; patch logged 120 frames after load; save/reload a patched state |
| 2 | Key walker + `coverlog`, nothing written | slot 7: planks as candidates; key steady idle, changes on open/close/fades; < 0.3 ms |
| 3 | Render-thread proof and tag plumbing | slot 7 covered; slots 1-4 not; tags match |
| 4 | `covershadow` | "would arm" only in full-screen menus; no mispredicts |
| 5 | Real skip, restore, watchdog, `BeforeSave` | slot 7 main target near 14.7 ms, identical; `covermagenta` clean through open/close/scroll |
| 6 | Other menus | item box, Gallery and cutscene, pause menu, equipment, smithy, map, guild card, loading fades |
| 7 | Optional: ETC1A4 opacity; skip lower groups | area counter drops; picture identical |

Close-frame checks from step 5: capture the frames around each close against `nocoverskip`; keep a
menu open 30 s then close (animations, effects, shadows, sound); save with a menu open, reload,
close. Slots 1-4 must show `cov a0`.

## 7. Status 2026-11-19: steps 1-5 built and tested (opt-in `covertrap`)

Built as planned with these differences (code: `mh4u_hooks.cpp`, `common/mh4u_cover.h`,
`rasterizer_cache/tile_opacity.h`, the proof in `gxm_rasterizer.cpp`):
- **Walk**: direct host-pointer reads; a group whose signature (identity, order, position, colour,
  flags without 0x200) is unchanged re-reads only its last candidates, and is skipped entirely when
  the game marks it unchanged (`0x100`, not `0x200`, `+0x42` 0, `+0x0C` 0, same `+0x40`); one
  group walked in full every 8th trap as a check (`nocoverrev`, `nocovercache` turn these off).
  Counter 0.25-0.34 ms mean (was 1.5), field 0.07-0.18 ms.
- **Tag**: a never-reset trap counter read when the top display transfer is queued, carried to the
  render thread with the transfer (`SetFrameTag`); every rule allows a lag of 2.
- **Proof**: per-pixel (240x400 bits) instead of cells. Opaque quads cover their pixel centres;
  quads refused as a whole are judged in 8x8 blocks. World-dependent draws in the 2D layer (depth
  or stencil test, varying w, sampling a GPU-made surface whose producer used depth, lighting or
  copies) uncover what they touch; surfaces whose content came from guest memory (`gpu_content`
  false: e.g. the CPU-written page at the counter) and 2D-rendered surfaces do not.
- **Texture opacity**: 2 bits per 8x8 tile, built from the guest bytes at upload, kept valid by
  the upload hash (MH4U rewrites its static textures with the same bytes every frame, which bumps
  the modification tick), rebuilt lazily when stale. ETC1A4 / RGBA4 / RGBA8 / IA / A formats.
- **Decision**: arm needs a covered proof with tag >= key start + 3, < 30 frames old; disarm on the
  first trap where anything fails. Watchdog: 3 transfers without a trap.

Console results (release unless noted):
- Slot 7 (quest counter): arms ~6 frames after each menu change, 82 units hidden. Main target GPU
  14.7 ms per frame (diag `scenetime`; ~83 ms without), 20-21 -> ~26 fps. Screenshot identical to
  `nocoverskip` except the fps overlay.
- Close to the world and reopen (Circle x6, X x3, three cycles): disarm "no cover key" on the close,
  world visible, re-arm on the wooden "here come the Quests" screens; 0 mispredicts in all runs.
- Slots 1-6: never armed (slot 6 Gallery menu does not cover the screen), 0 mispredicts.

Still to do before making it default: step 6 (item box, pause menu, equipment, smithy, map, guild
card, loading fades), `covermagenta` through open/close/scroll, save with a menu open and reload,
a 30 s open menu then close (animations, effects, sound).

Fix 2026-11-19 (user session at the quest counter flapped: 18-20 fps, key blacklisted after two
mispredicts): with the world hidden, MH4U leaves a texture unit enabled on the picture itself for
the plank draws (never read by their program), and the proof treated that as a target read, so
the planks stopped covering on exactly the armed frames. Now only units the program reads count;
depth write or an Always test no longer refuses a cover; covers drawn after the last world draw
but before the grade carry into the layer; arming needs 4 covered proofs in a row.
