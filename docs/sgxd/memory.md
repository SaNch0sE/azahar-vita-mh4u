# Memory, the MMU and the system-level cache

Every access the GPU makes, from any unit, goes through the memory interface: an MMU of the GPU's
own, then the system-level cache, then memory.

## The MMU

- **Two levels, 4 KiB pages.** A directory points at page tables, and an entry holds a physical
  page and its flags: valid, read-only and cache-consistent (below). The requestors (the vertex,
  pixel and event data masters, the 2D unit) each name the directory they use; sgxd gives every
  one of them the same directory, over sgxd's own tables.
- **Faults.** An access through no mapping stops the unit that made it. The interface latches the
  address and the requestor (a shader core's data cache, a texture read, the parameter buffer, and
  so on). A fault at an address that looks like a float or a
  half-float is usually a program that read a pointer from data.
- **Translations are cached.** After an entry changes, an MMU invalidate must come before
  anything uses it. sgxd changes entries in only two ways:
  - with the GPU idle, at setup and teardown, and when a pool or block is mapped during the run.
    The map invalidates the MMU's caches but not the system-level cache below, which the MMU's
    table walks go through: the cache is written back and emptied too before the GPU runs again.
    Without that, a 2D unit fill as the first work in a new block faults at its first page now
    and then;
  - from the microkernel, in queue order: the tables themselves are mapped for the GPU, so the GPU
    stores the new entries and invalidates its MMU between one job and the next. Work queued
    before the change reads the old page, and work queued after reads the new one; nothing waits
    for the GPU to go idle. The emulator moves guest pages between physical pages this way.

## One address space, built so the draw path translates nothing

- **Guest memory appears at the PICA's physical addresses.** The 3DS's VRAM and main memory
  (FCRAM) are mapped for the GPU where the PICA sees them, so a vertex buffer, texture or render
  target address from a guest command is a GPU address as it is. Draws read vertices and 16-bit
  indices where the guest put them, and renders write the guest's buffers in place.
- **Video memory (CDRAM) for what the GPU writes.** Guest VRAM lives in CDRAM, and the guest
  main-memory pages the GPU renders into, reads textures from, or the display shows are moved to
  CDRAM at the same addresses, for the guest, the emulator and the GPU alike
  ([../native-execution.md](../native-execution.md)). A moved page the GPU stops using
  goes back.

## Video memory

- **Uncached for the CPU.** The CPU's writes to CDRAM go straight through, and the GPU's writes
  need no invalidation on the CPU's side. That is why everything the GPU writes and the CPU reads
  lives there: no cache maintenance on either side of the GPU's work. The price is the CPU's own
  access: every store is a bus transaction, and every read waits for the bus, at about a fifth of
  cached memory's rate.
- **What the CPU reads again and again there is kept in cached memory.** Games keep command
  buffers in guest VRAM that their lists jump into every frame, and the emulator reads each one
  as it takes a list. Those words are kept in a cached copy and read from it while nothing has
  written them. Every write is seen: the pages are closed to the guest, so its stores trap, and
  a DMA or a GPU operation announces the bytes it will write when it is queued. A written word
  is read from VRAM again, after the queued writes over it. Read from VRAM instead, a game that
  draws a scene from 150 KiB of such buffers it never rewrites reads 8 MiB a second out of it, a
  quarter of its emulation time.
- **Budgeted.** The emulator has all 128 MiB of CDRAM while sgxd owns the GPU, the shell's 16 MiB
  included ([takeover.md](takeover.md)). A failed allocation stops the emulator, with two
  exceptions. The moved guest pages' pool grows into uncached main memory once CDRAM is full. And
  a full pool of texture copies drops copies, since each can be made again from the guest's
  texture: never one the open render reads, and the stretch that frees a run of the size wanted
  for the fewest bytes dropped. Only a block that still finds no room goes to uncached main
  memory, where the CPU needs no cache maintenance either and only the GPU's reads are slower.
  CDRAM fragments into the renders' blocks, so a late, large block can find no run even with
  megabytes free in all.

## The system-level cache

A cache between all four cores and memory, shared by every unit.

- **A GPU store reaches memory only when the cache writes it back.** A flush (of everything, or of
  one data master's lines) is a register write, acknowledged in a status register; it raises no
  interrupt, so whoever waits for it polls. Without a flush, a program's stores can sit in the
  cache indefinitely.
- **A program's last store is no fence for its earlier ones.** The CPU can see a marker the
  program stored last while stores made before it are still in the cache.
- **An invalidate while work stores drops those stores for good.** A line dirtied between a flush's
  acknowledgement and the invalidate is discarded. So: flush while the work runs, and invalidate
  only once the work is done. Sony's microkernel works in that order, and so does sgxd's.
- **Cache-consistent pages bypass it.** With the bypass setting Sony's driver programs, stores to
  pages mapped cache-consistent reach memory without a flush. sgxd maps the memory the CPU and the
  microkernel share this way: the job queue and its counters. The CPU's own data cache is not kept
  coherent by it: a CPU-cached line of such a page still has to be invalidated by the CPU before a
  read.
- **Sony's pattern around each unit.** Sony's microkernel flushes a unit's lines before it reports
  that unit's work complete, and flushes and invalidates in a fixed pattern around the binning and
  shading runs. sgxd follows the same pattern; the 2D unit's operations take no cache requests
  around them.

## The units' own caches

Each core caches what its units read, and none of these caches sees the CPU's writes:

- **Data:** what the data mover fetches. A line fetched once is returned stale after the CPU
  rewrites it, until the data cache is invalidated. Lines it never fetched come fresh: it
  prefetches nothing.
- **Code:** the shader cores' instructions ([usse.md](usse.md)). The data mover's own programs
  follow a stricter rule ([pds.md](pds.md)).
- **Textures and parameters:** invalidated by sgxd in Sony's pattern: the parameter caches before
  every render, the data and texture caches before every run.

Guest main memory the CPU caches needs its lines cleaned before the GPU reads it. Vertex and index
buffers under the page holds stay in cached memory, where the guest's writes are fast: a streamed
model unpacked into the same pages again and again would have every refill written through an
uncached view, byte by byte. Their pages are cleaned when they have been written, not for every
render:

- A draw's read of a page closes it to the guest's writes. A page closed since its last clean has
  nothing new in the CPU's cache, and the draw reads it as it is.
- A page written since (it was open, or a store went past the hold beside the bytes read) is
  cleaned once, on the emulation thread, before the draw's list goes to the render thread.
- A guest write to a page that queued draws still read does not wait for them: the guest gets
  another page of cached main memory with the same bytes (a version, as a page moved to CDRAM gets
  one there) and goes on at once, and the next draw that reads the page moves the GPU onto the
  written version and cleans it.

Any other GPU use of such a page (a texture, a render target, a transfer) takes its versions home
first: those pages move to CDRAM with their work. 8-bit index lists are widened by the CPU into the
render's own data: the GPU never reads them in place.

Geometry outside the page holds (the game's resource files and the buffers it hands over frame
by frame) is cleaned for every render that reads it, and the next vblank moves its pages to CDRAM,
so later renders need no clean.
