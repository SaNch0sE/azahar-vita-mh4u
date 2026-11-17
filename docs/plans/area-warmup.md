# Area crossing: a few seconds to climb back to 30 (plan, 2026-11-17)

User report (release 0.3 candidate): moving to another area dips, then climbs back to 30 over
about 5 s. Planned with Fable; not started.

## Likely causes, ranked

1. **Cold program cache.** `programs.bin` is versioned by an MD5 that includes
   gxm_rasterizer.cpp (src/video_core/CMakeLists.txt), so almost every build starts it over, and
   the diag build's version also hashes gxm_flags.txt, so every diagnostic flag run wipes it too.
   On a cold visit lit programs run as tier 1 until 100 binds (gxm_rasterizer.h tier-2
   threshold), then wait for the tier-2 worker, then their pipelines are built a second time.
   Both tiers are stored and a warm visit starts on tier 2 (ProgramDiskCache::Find).
2. **Warm residue:** pipelines are never persisted (built at first bind on the render thread),
   and disk-cache reads are synchronous on the render thread while LayeredFS also reads.
3. **Vertex cache re-warm:** a load's writes make reused keys volatile for 10 s.
4. **New mod build:** LayeredFS open-file LRU of eight, synchronous reads under 64 KiB.
5. Shader JIT compiles; 6. texture uploads; 7. a steady 16:9 cost; 8. heap pressure.

## Measure first (release, trans.sh; no diag flag runs in between - they wipe the cache)

Check the card for switch files; back up programs.bin. Find a crossing from slot 6/7.
R1 cold (programs.bin trashed), R2 and R3 warm (same route), R4 with the previous mod if R2/R3
still dip. Per crossing: seconds to shown 30; game vs shown fps (game < 30: emulation side,
causes 3-5; game 30 and shown < 30: render/GPU, causes 1, 2, 6); `health` deltas (shaders,
retired, pipelines, surfaces, heap), `cpu`, `fs`, `stall`, `vc`. Diag follow-up for cause 1
(with programs.bin backed up): `nodiskprograms`, `nodiskprograms tier2now`, `nodiskprograms
notier2`.

## Fix options

- F1 diag flag runs use their own cache file (no more wiping the release cache).
- F2 version the cache by an explicit key epoch instead of the rasterizer's source hash.
- F3 a lower tier-2 threshold (8-16 binds), or tier 2 queued at emission for lit programs.
- F4 pre-build the replaced shader's pipelines at the tier-2 swap.
- F5 ship a pre-built programs.bin in the vpk (made by a scripted tour of the release build).
- F6 read the program cache off the render thread.
- F8 emulator-made writes don't count toward vertex-cache volatility.
- F9 LayeredFS: larger open-file LRU, more reads on the worker.

Confirm with R1-style cold runs (target: shown 30 within ~2 s of a crossing), R2-style warm runs,
heap and pipeline counts after eight crossings, and fsum.sh over the logs.
