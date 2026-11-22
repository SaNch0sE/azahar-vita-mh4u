# Debugging and profiling

Use the release debug build for speeds and CPU profiles, and the diagnostics build for asserts and
the GPU's numbers ([building.md](building.md)). Both read launch arguments and a command channel for
savestates, settings and quitting; they are listed where they are parsed
(`src/citra_vita/citra_vita_main.cpp`).

## Crashes

A crash azahar notices itself (an assert, an abort, a GPU stop) writes a report with the build, the
console's plugins, the settings and the console's last lines, takes a core dump of its own process,
restarts on the game list, and packs both into `ux0:data/azahar/crashes/crash-DATE-TIME.zip`. A
player's zip is all there is to go on, so the report says what their console ran.

- `tools/symbolize_crash.py REPORT ELF_OR_DIR`: the report's call chain and return addresses
  named, from the ELF whose build ID the report carries (keep each release's ELF).
- A core dump holds stacks and a few pages, not the heap or the GPU's state.
- A fault in the guest's code reports the guest's registers and how each core's tables map the
  address. `tools/guest_decomp.sh` decompiles the title's functions around it from a dump of its
  code (the diagnostics command `vdump`).

## GPU stops

sgxd never resets a hung GPU while it runs: a hang stops the emulator, which restarts as after any
crash and takes the GPU again from a reset. The log's last lines say where each of the
microkernel's two streams stood, which job each ran, and each core's memory fault (the unmapped
address and who read it). `tools/sgxd_dump.py` decodes the diagnostics build's register dump and
names a core stuck in the clipper ([sgxd/ta.md](sgxd/ta.md)).

## Wrong pictures

Check upstream Azahar on the same scene first, and the build before for a regression. Then record
the frame with the diagnostics build (`tools/frame_replay/FORMAT.md`): the replay player runs that
frame alone, checks every pass's picture against the recording, shows the picture after any draw,
and times each pass and draw. A wrong picture that comes and goes usually points at the late skip or
at a page the GPU read while the guest rewrote it: the diagnostics switches that serialise the guest
and the GPU, or log the skip, tell them apart. Compare a picture's colour channels: a check on the
alpha channel alone passes programs that draw wrong.

## CPU profiles

An emulation thread spends its time in four places: the guest's code, the emulator's code called
from the guest's world (upcalls), the emulator outside it, and Sony's modules. Which thread runs on
which core: [threads.md](threads.md).

## GPU profiles

The diagnostics build's `perflog` prints each second's numbers: the GPU's load and time by who
sent each render, the render thread's busy time and waits, the threads' run times, presents and
frames left out. Per-pass GPU times come from running every pass alone (`syncpasses`) or from a
frame replay.

## Speed

- Release debug builds, vsync off and no speed limit, from the same savestate for every build
  compared.
- Report the game's frame rate and the shown frame rate together: a GPU-bound game leaves frames
  out rather than slowing down, so it runs more frames than it shows.

## Mechanisms

A cost or a bug in one mechanism is measured or found with a test homebrew rather than a game: the
3DS homebrews in `tools/` each exercise one feature (their headers say what and how they report),
and `src/sgxd_test/` (diagnostics, `sgxd_test.vpk`, results in `ux0:data/sgxd/`) checks each
hardware rule sgxd depends on against the CPU's result.
