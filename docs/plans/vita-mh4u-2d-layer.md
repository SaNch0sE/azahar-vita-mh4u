# MH4U's 2D layer: how the game builds it, why menus cover ~5 screens, and game-aware fixes

Research 2026-11-19 (Fable), read-only. Sources: the game code (disassembly of our mod-built
USA v1.3.0 executable), the layout files of the same mod build (parsed with the Heavenfall mod's
own readers), the Heavenfall mod source (addresses in EUR space), and MH4UltimateDC
(https://github.com/CompaLarios18/MH4UltimateDC, facts only). Code addresses are USA v1.3.0 (our
"v1.1"); object offsets are the same in every version.

## 1. Verdict

- The ~5 screens of 2D at the quest counter are stacked, nearly opaque panels, all drawn blended:
  two full-screen wood layers, with the 3D village drawn under both. The layout files sum to
  ~5.5-6 screens.
- The Heavenfall single-screen layout causes most of the stacking: it puts the touch screen's
  quest list on the top screen, stretches its wood board over the whole screen, and adds its own
  full-screen fill to the quest-detail layout.
- The game never culls: hidden sprites (and sprites under hidden nulls) are submitted as
  zero-size points, alpha-0 sprites are drawn at full size, static groups are replayed every frame.
- Bottom-screen (view 7) groups are not drawn into the top target; with `srcbottom` the view-7
  pass is not built at all.
- Game structure does not explain the ~24 GPU cycles per blended pixel; it rules out texture
  memory for the big panels and mask passes at the counter.
- Best fixes: hide what lies under a full-screen opaque cover at the source (the 3D world and the
  lower groups), drop zero-area sprites in the emulator, cache static groups using the game's own
  change flags.

## 2. How the game composes a 2D frame

### Objects

| Object | Size | Fields |
|---|---|---|
| `sMhLayout` (`[0x01057534]`) | 0x248 | `+0x100` main `uLayout*`; `+0x245` top view 0; `+0x246` bottom view 7; `+0xB4` draw state |
| `uLayout` (`cUnit`, vtable `0x00e6bb40`) | 0x198 | `+0x04` unit flags, bits 16-25 `mDrawView` (bit 16+view); `+0x120` draw list head; `+0x154` group array; `+0x160` group count |
| Root group | 0x50 | `+0x00` hash `~crc32(name)`; `+0x04` name; `+0x08` first pane; `+0x0C` redraw queue; `+0x18` order (bits 27-31 layer, 19-26 priority); `+0x1C` two cached command lists; `+0x28` x, y, z; `+0x3C` RGBA; `+0x40` revision; `+0x44` flags; `+0x48` next |
| Pane | 0x24 | `+0x08` data; `+0x10` kind (0 sprite, 1 null, 2 text, 3 boundary, 4 window); `+0x11` depth; `+0x14` next; `+0x20` child |
| Sprite | 0x68 | `+0x00` texture; `+0x10` x, y, z; `+0x20` size; `+0x28` scale; `+0x30` uv rect; `+0x40` four RGBA corners; `+0x58` bits 0-3 origin, bit 7 visible, bits 8+ vertex slot; `+0x60` low 11 bits sort sub-key |
| Null | 0x30 | position, `+0x10` scale, `+0x18` colour, `+0x24` bit 0 visible, bits 1+ matrix slot |
| Text | 0x80 | `+0x5C` bits 24-27 outline layers; glyphs are sprites under a null |

Group flags `+0x44`: `0x100` cached, `0x200` command list re-recorded this frame, `0x400` shown,
bits 14-17 view, bits 18+ index. Revision `+0x40`: low 16 bits sprites submitted at the last
build, byte `+0x42` buffers to rebuild, bits 24-30 a change counter.

### Draw path

1. `uLayout::draw` `0x00b04774` walks the draw list; a group is drawn only if `0x400` is set and
   its view equals the view being drawn. The mod replaces `0x00b047cc` with its own view function
   (a hidden block gets view 15). The per-group call is `bl 0xae6cd0` at `0x00b047e0`.
2. Group draw `0x00ae6cd0`: uncached (flag `0x100` clear or change counter >= 4) - constants
   (`0x00ae65a4`), walk all panes (`0x00ae5650`), flush, every frame; cached and dirty - record a
   command list, set `0x200`, replay; cached and clean - redraw queue (`0x00ae54c0`), constants,
   replay (`0x0062c9d8`), clear `0x200`.
3. Pane walk `0x00ae5650`: sprites written by `0x00ae7044` with no visibility test (a sprite with
   bit 7 clear gets size zero: still a point, zero area); nulls by `0x00ae8db0` (an invisible null
   gets a zero matrix, its children are still submitted, collapsed); text by `0x00ae5f30`, each
   character `layers + 1` glyph sprites.
4. Batching (`0x00ae5978`): a run continues only while the next sprite has the same sort sub-key,
   textures, pane depth and the next vertex slot - hence ~1.1-1.7 points per draw.

| Question | Answer |
|---|---|
| Hidden group drawn? | No |
| Hidden sprite / null drawn? | Yes, as zero-area points (vertex and GS work, no pixels) |
| Alpha-0 sprite drawn? | Yes, full size, blended (the mod hides panes this way) |
| Off-screen culled? | No test seen (medium confidence) |
| Static elements redrawn? | Yes; the cache saves guest CPU only |
| Panels as small sprites? | Yes: centre fill + board + frame pieces; paper sheets 5-10 pieces, each drawn twice (sheet and shadow) |
| View 7 in the top target? | No |
| Mask passes | Minimap only |

## 3. The quest counter's ~5 screens

The mod's `single_screen.json` gives `ui409` (quest list) a backdrop `ui409_boad_haikei` stretched
over the top screen, and `ui408` (quest details) a fill sprite `ss_fill` as its first child.

| Group | Visible / total sprites | Area (screens) | Largest parts |
|---|---|---|---|
| `ui408_sitaji` | 106 / 199 | ~2.7 | `ss_fill` 427x240 alpha 255; `ui408_ita` 178x182; paper pieces with shadows |
| `ui409_boad_haikei` | 8 / 12 | ~1.5 | planks 320x120 x2, 107x120 x2; mark 213x160 alpha 220 |
| `ui409_list` | 112 / 155 | ~1.5 | paper pieces, shadow copies |
| `ui409_q_menu` | 22 / 22 | ~0.4 | |
| `ui409_ticket` | 13 / 216 | ~0.1 | 203 hidden sprites still submitted |
| Text | 141 panes | ~0.5 | |

The planks (ETC1) and `ss_fill`/`ui408_ita` (an opaque ETC1A4 region) are opaque: one full-screen
wood layer is entirely hidden by the other, and the 3D village by both. The same pattern recurs:
32 groups hold at least one near-full-screen sprite (`ui002_base00`, `ui004_*`, `ui008`, `ui033`,
`ui492`...).

## 4. Other findings

- Largest alpha-0 (hidden-by-the-mod) areas in the files: `ui511_BG` 153,600 px, `ui497` 98,400,
  `ui705_nunoback` 97,284, `ui801_l_bg` 96,000, `caplogo_cap` 96,000, `ui500_PL_list` 87,166,
  `ui803_bg` 76,800, `ui800_l_uraita_p/o` ~69,000/~67,000. At the counter only 2 x 1,183 px.
  Measured (release counter `hudres ... z`): quest counter 0.6% of the 2D area at alpha 0; camp
  HUD 30% (~25,000 px a frame).
- 2D blend: colour Add(SrcAlpha, OneMinusSrcAlpha), alpha Add(One, Zero) - destination alpha is
  overwritten with source alpha (the minimap reads it), so culling alpha-0 sprites in the emulator
  is exact only when no DstAlpha draw follows in the frame.
- MH4UltimateDC has no native 2D drawing; its layout hook only shifts HUD translations.
  Heavenfall changes placement, not drawing.

## 5. Fix options

| # | Fix | Saves | Risk | Effort |
|---|---|---|---|---|
| 0 | Layout dump from game data (`layoutlog` in the hook module) | sizes everything | none | 0.5 day |
| 1 | Drop zero-area sprites in the emulator | CPU on GX, worker, render threads | low | 1 day |
| 2 | Skip what a full-screen opaque cover hides, at the source | ~22 ms GPU (world) + >= 1 of 5 screens of 2D + CPU | medium | 3-5 days |
| 3 | Mod data: drop the redundant full-screen layer; hide by scale 0 / visible bit, not alpha 0 | ~1 screen at the counter; alpha-0 areas elsewhere | low | 0.5 day |
| 4 | Cache static groups using the game's change flags | most 2D CPU and GPU on idle menus | medium-high | 1-2 weeks |
| 5 | Blend off for provably opaque sprites | little once 2 is in | medium | 3-5 days |
| 6 | Merge sprites across sort keys (`ui-draws.md` R2) | CPU | medium | as planned |

Step 0 walks `[[0x01057534]+0x100]+0x120` along `+0x48` once a second on a switch, logging per
shown view-0 group: name, layer, priority, flags `0x100`/`0x200`, change counter, sprite count,
alpha, visible and alpha-0 areas, hidden sprites, largest sprite. Its area sum must match
`hudres ... a` within ~10%.

Option 2: after the game's update and before its render, find the top-most shown view-0 group
with a visible sprite (corner alphas 255, group and null alphas 255, opaque texture region)
covering 400x240; then 2a clear bit 16 (view 0) of `unit+0x04` for every non-layout unit in
sUnit's 32 lines (`[0x010572F8]+0x14`, 0x18 each, first unit at `+0x0C`, next at `+0x0C`) and
restore it when the cover goes (data only); 2b skip groups below the cover (with the mod the view
is computed at draw time, so a word-verified patch of the call at `0x00b047e0` to a stub). Must run
between update and render (the mod's site `0x00af550c`; a neighbour like `0x00af551c`). First
experiment: at slot 7 clear bit 16 on all non-layout units once - the main target should drop
~22 ms with an identical screenshot. Risks: `mDrawView` gating all units is inferred; a unit doing
bookkeeping in its draw; v1.0 needs its own code addresses.

Not confirmed: draw order of `ui408_sitaji` against `ui409_boad_haikei`; run-time sprite counts
(file state only); `sUnit`'s draw function; the cause of ~24 cycles per pixel (the blend-off test
E5 of `vita-mh4u-hud-cost.md` decides).
