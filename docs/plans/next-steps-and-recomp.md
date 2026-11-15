<!-- Remaining improvements and the static-recompilation idea (Fable, 2026-11-15). Estimates unless marked measured. -->

# Remaining improvements, and whether a recompilation of MH4U would help

## Ranked candidates
Gameplay (GPU-bound, ~30-31 ms of 33.3):
1. Measure more scenes first: a large-monster fight with effects, a cave or night area, a water or lava area (save states needed).
2. Non-dependent texture reads (G3/G4, docs/plans/gameplay-headroom.md): 1-2.5 ms; one-day spike on the fog strip first.
3. Remaining exact fragment trims (constant specular for one light, direct alpha compare, LUT row from the upload): 0.5-1.5 ms.
4. Per-texture minimum-alpha census for unblended GREATER 0/5/30 tests: 1-3 ms where they apply; census first.
5. Stall recorder for the rare one-second drops to ~13-15 fps.
6. Worker-thread file reads for the last ~0.1 s of an area crossing.
7. Vertex output trimming, depth-store skip, uniform write lists: 0.3-1.5 ms each.
Menus and quest counter (GX-thread-bound, ~880 ms/s):
8. GX shipping trims (menu plan Phase 2): 40-70 ms/s, ~1-2 fps.
9. Hidden village behind opaque menus (Phase 3): ~24% of the render thread; 1-2 weeks.
10. A game-side patch that stops the village being submitted under full-screen menus: the only option that relieves the GX thread too.
TuizaVita: only Z-order textures remain; expected under 0.5 ms, left.

## Static recompilation (github.com/fearkov/3dsrecomp)
- A Rust work in progress translating ARM/Thumb/VFP to C for the author's own emulator (Zakuro); tested on a few titles, not MH4U; threads, SVCs, services and GPU stay in Zakuro; Linux/Windows only.
- Not faster here: the game's code already runs natively on the Cortex-A9; generated C with a register context and a memory model adds overhead. The time is in the GPU (areas) and in PICA translation on the GX thread (menus), which a recompilation does not touch. mh3-wii needed one because PowerPC cannot run on ARM; its speed came from its native renderer and hand-ported hot functions.
- On the Vita itself it is unrealistic (compiler and linker on the console, ~13 MB heap free); on a PC it is legally clean but costly for every user and a moving dependency.
- Recommendation: not needed. The cheaper version: Ghidra on the owner's own decrypted code.bin, guided by guest return addresses logged at the GX submit, for one to three targets (first: the village drawn under full-screen menus), shipped as small address patches (cheats/IPS or native hooks like src/core/arm/vita/mh4u_vector_hle.h) keyed to USA + update v1.1, with a kill switch each. No game code in the repo.
