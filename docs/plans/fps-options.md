<!-- What limits fps: options survey (Fable, 2026-11-15). Model-written plan; estimates unless stated as measured. -->

# MH4U on Vita: what would really move performance

Nothing here was run on the console. Everything is from the docs, the code and the logs in the scratchpad. Measured facts and my estimates are labelled.

## Headline

Your alpha-test logs do not support "the discard itself costs ~4.8 ms". They show the alpha test causing about 40% more fragments to be shaded, whatever the program costs. That makes a depth pre-pass submitted at the start of the scene worth about 6 ms of GPU, not the 2 ms the doc projected for an in-place pre-pass.

Per-frame GPU on the main target, recomputed from the `al_st_*` and `al_x_*` logs (measured):

| Programs kept | Fragment ms without alpha | With alpha | Added |
|---|---|---|---|
| Vertex colour only (`dbgminfs`) | 3.8 | 4.9 | +1.1 (29%) |
| + textures + TEV | 11.6 | 16.2 | +4.6 (40%) |
| + lighting | 18.7 | 26.6 | +7.9 (42%) |

- **The cost scales with the program, so it is overdraw.** A fixed per-discard cost would add the same amount in all three rows.
- **`ptmin` points at lost hidden-surface removal.** Trivial discard programs in a lit world cost 22.4 ms of fragment time against 18.1 for `ptskip`; only ~1.1 of that 4.3 ms is the trivial program itself. My inference for the other ~3 ms is that each punch-through object makes the SGX shade what is already tagged, so lit opaque fragments that end up hidden get shaded anyway.
- **The alpha-tested draws are the grass and the hunter.** `al_x_ptalways.jpg` shows the grass as stacked, screen-covering sheets; `al_x_ptskip.jpg` loses the hunter.
- **The early kill gaining under 1 ms fits this.** The wasted fragments mostly pass the alpha test or are opaque.

## 1. Bottleneck map

| Scene | Binding limit | Evidence |
|---|---|---|
| Area 3 | GPU: 33.3 ms a frame against a 33.3 ms budget. The render thread is at ~94%, but ~5 ms of that is waiting for the GPU. The GX thread and the guest are not limits. | `al_a3st_log.txt`: `scene GPU time 1672944 us … 100 scenes` over 50 frames. `al_a3rel_log.txt`: `game 30.0 fps shown 28-29 fps gx ~400 ms`. |
| Area 3, shown 28-29 | Frame pacing, as a symptom. Flips are vblank-quantised, so a 34 ms frame lands a vblank late and the frameskip drops one. | `src/citra_vita/gxm_present.cpp:716` (`sceDisplayWaitVblankStart`), `src/video_core/gpu.cpp:1563` (skip decision). |
| Quest counter | Render-thread CPU first, GX thread second. The GPU is idle and the emulation thread is ~35% busy. | `al_f0_on_log.txt`: `render thread was busy 2916291 (97%)`, `GPU wait is 3737`, `software vertex shading: 36085 draws, 1451288 us (48%)`, `guest 316461 us`. `qb_relphi_log.txt`: `game 24-26 fps gx ~880 ms`. |
| Hub / village | At the 30 cap (29.5-29.8 shown); the render thread is the next limit. One rare stall of unknown cause. | `docs/vita-mh4u-performance.md`, village sections. |
| Clocks | The code asks for ARM 444 MHz, GPU 222, bus 222, Xbar 166. The doc header says 500 MHz, but nothing reads the clock back. | `src/citra_vita/citra_vita_main.cpp:297-300`. |

## 2. Ranked options

**1. Alpha pre-pass at the start of the scene (GPU, area 3 and any foliage area).**
- **Change:** for alpha-tested draws that write depth with a LESS-type test and One/Zero blending, draw them twice.
  - First, at the start of the scene: colour mask off, depth write on, an alpha-chain-only kill program. The eligibility analysis already exists in the scratchpad `gxm_alpha_prepass.cpp`.
  - Then, in their original position: the no-kill program, depth EQUAL, no depth write. Every colour draw is then opaque and gets full hidden-surface removal.
- **How to get the order:** record the main scene on a GXM deferred context and issue pre-pass draws straight to the immediate context, then `sceGxmExecuteCommandList` before `EndScene`. The API is in the vitasdk header (`psp2/gxm.h:1549-1662`). It needs no lookahead. Hooks are `BeginScene`, `EndScene`, `BindPrograms` and `SyncState` in `src/video_core/renderer_gxm/gxm_rasterizer.cpp` (lines 834, 1210, 1441, 1279).
- **Gain (estimate):** 33.3 ms to about 27 ms. That is `ptalways` at 25.1 ms, plus ~1.1 for trivial punch-through fragments, plus ~0.8 for the second vertex pass. Range 4-7 ms. Shown fps would hold 30, and the render thread gets its ~5 ms of GPU wait back.
- **CPU cost:** ~33 extra draws, about 1.5-2.5 ms of render thread a frame.
- **Effort:** 5-8 days.
- **Risk:** medium-high.
  - Deferred contexts are little used in homebrew, and the heap is tight (193 of 205 MB).
  - The result at exactly equal depths can differ; restrict eligibility and keep a kill-switch file.
  - EQUAL needs bit-identical depth, so use the same vertex program binary for both passes.
- **Verify in steps:**
  1. Census, half a day: log the alpha-test function per draw in the sequence dump. `seq_a3.txt` lacks it, so I could not tell whether the alpha-tested draws are contiguous.
  2. In-place variant, about 2 days. I expect about 30.5 ms, the same as the doc's projection. If that holds, the model holds.
  3. Deferred-context version, with `scenetime` and a magenta-on-mismatch check mode.

**2. Confirm the CPU clock; ask about 500 MHz (owner decision).**
- **Change:** log `scePowerGetArmClockFrequency()` at boot, one line.
- **Gain (estimate):** if it reads 444, going to 500 through PSVshell or your own `vita_native` plugin is about 12% on every CPU-bound thread. At the quest counter that is roughly 25 to 27-28 fps.
- **Limits:** it does nothing for area 3. I know of no way to raise the GPU clock above 222 MHz.
- **Effort and risk:** an hour; none for the readback.

**3. Texture-only fast path for software draws (menu plan F0 + A; render thread).**
- **Status:** still not in the tree. `texdimsalways` and `notexrepeat` do not appear in the source.
- **Gain (plan's estimate):** 110-175 ms/s of render thread; counter 24-26 to ~27-28 fps. It also covers ~27 HUD setups a frame in area 3.
- **Effort and risk:** 1-2 days; low. Verify with `texrepeatcheck`.

**4. Geometry-shader point draws on the GPU (the other agent's plan).**
- **Gain (my estimate):** 3.7 ms a frame of render thread in area 3 (measured feed plus draw cost). At the counter it is potentially the largest render-thread item, since software draws are 48% of the thread.
- **Sequencing:** do option 3 first. It is cheap and its counters size what is left.

**5. Hidden village behind full-screen menus (menu plan B; render thread and GPU).**
- **Gain (plan's estimate):** about 24% of the render thread at the counter.
- **Effort and risk:** 1-2 weeks; medium-high, and it rests on assumption H1.
- **What it does not fix:** the GX thread still parses the village, about 110 draws at ~73 us, roughly 8 ms a frame.
- **Game-side alternative:** only a guest patch that stops MH4U submitting the world would relieve the GX thread too. The gain is larger, but it needs reverse engineering of unknown size and fades are a glitch risk. I would use it only as an arming hint for B.

**6. GX shipping trims (menu plan G).**
- **Gain (plan's estimate):** 40-70 ms/s.
- **When:** needed for 30 fps at the counter once the render thread stops braking. GX real work is ~650 ms/s at 25 fps, so ~780 at 30.
- **Effort and risk:** 1-2 days; low.

**7. Frameskip signal fixes and release counters (area plan FS1, FS2, section 5.4).**
- **Gain (plan's estimate):** 0-1 shown fps. It also makes the release `render ~N ms` figure trustworthy; today it swings from 587 to 1231 ms in one log.
- **Effort:** half a day each. Do the counters before option 1 so the result is readable.

**8. Stall recorder.**
- **Change:** in release, when a frame exceeds ~60 ms, log each thread's current phase marker and the heap.
- **Why:** the stalls to ~15 fps have no known cause, and the heap margin is a suspect.
- **Effort:** half a day. No gain by itself.

**9. Residual bottom-screen scene (area plan S).**
- **Evidence:** every frame still has `0: 180D4800 256x512 1 draws, 1: … 240x320 1 draws (blit)`.
- **Gain (plan's estimate):** 0.5-1.5 ms of GPU. Effort half a day; low risk.

### User-choosable trade-offs (visible, off by default)
- **Per-vertex lighting or fog on alpha-tested foliage only:** up to ~3 ms of GPU (estimate), with subtle shading differences.
- **Lower-resolution main target:** about 8 ms at 70% of the pixels (estimate), but the HUD shares the target and blurs. I would not offer it.

## 3. Tempting, but won't help

- **More early-kill or instruction trimming in lit programs.** Measured small (0.6 ms for the early kill); the cost is the number of fragments shaded.
- **In-place pre-pass as the final answer.** It leaves the pass-break waste; about 3 ms, not 6.
- **Static alpha resolve or per-texture opacity verdicts.** The costly alpha-tested surface is real cut-out grass. Only the hunter might qualify, and it covers little of the screen.
- **`FUNC_NONE` blending, depth load/store, hardware-filtered LUTs.** Measured at zero or ruled out in the doc.
- **GX work in area 3.** That thread is ~40% busy there.
- **Skipping the shadow-map pass or post effects on the game side.** The render thread already drops them (`408 skipped` per 51 frames); only idle GX time would be saved.
- **GPU overclock.** Already at 222 MHz, with no known way higher.

## Uncertainty

- **The pass-break mechanism is inferred, not observed.** It comes from cost scaling with the program and from the `ptmin` residue. The in-place step is the cheap falsifier.
- **Deferred-context behaviour is unproven here.** I have not checked its CPU or memory cost in this libgxm setup.
- **Counter figures compound.** Options 2, 3, 4 and 6 overlap; I would expect 30 fps there only after 3 and 4 plus either 5 or 6.

### Critical Files for Implementation
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/renderer_gxm/gxm_rasterizer.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/renderer_gxm/usse/fs_usse_gen.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/citra_vita/gxm_present.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/video_core/threaded_rasterizer.cpp
- /home/areki/Documents/Personal/grarakAzahar/azahar/src/citra_vita/citra_vita_main.cpp
- Prototype pieces outside the repo: /tmp/claude-1000/-home-areki-Documents-Personal/f3394e24-c6d1-4869-8501-c260051877ee/scratchpad/gxm_alpha_prepass.cpp and /tmp/claude-1000/-home-areki-Documents-Personal/f3394e24-c6d1-4869-8501-c260051877ee/scratchpad/alphaexp.patch
