# sgxd against Sony's driver

Sony's GPU stack has three layers: libgxm in the application, a kernel driver that turns its
requests into commands, and a microkernel on the GPU that runs them. sgxd replaces all three while
it holds the GPU ([takeover.md](takeover.md)), and departs from them where Sony's choices serve
Vita games, many processes or recovery.

## The stack

| | Sony | sgxd |
|---|---|---|
| **Clients** | every process, one call at a time under one lock, each with its own render contexts, render targets and GPU address space | one process, the emulator, for as long as it holds the GPU; other processes wait at Sony's gate |
| **Who builds a render's registers** | the kernel, from a compact kick record per render: it checks the target, derives the region clip, the binning and depth flags and the macrotile layout, and writes a command | the user library, once per target size and kind; a render is a prepared shape plus the few values that change, sent straight to the GPU's queue |
| **Render targets** | objects created in advance, with their tile layout | none: a target is a handful of words in a prepared render |
| **Shaders** | compiled offline, patched at bind time with the blend and the output format | written as machine code at run time by the emulator ([usse.md](usse.md)) |
| **Data-mover programs** | built by the shader patcher per shader, at bind time | built for each new set of a draw's inputs and shared by the draws with the same, in memory used in turn under fresh GPU addresses ([pds.md](pds.md)) |
| **GPU memory** | an address space per application, mapped by the CPU at the application's request, in fixed heaps | one directory for every requestor, guest memory at the PICA's own addresses, and page tables the GPU itself edits in queue order ([memory.md](memory.md)) |
| **Waiting** | the interrupt wakes a driver thread, which wakes each waiter's event | the render thread arms the counts it waits for; the GPU interrupts only when one is passed |

## The microkernel

Both run their queue on core 0's shader core, started by events, and neither waits for a render
inside a run: a binning run's or render's end is a new event. Past that they share little.

| | Sony's | sgxd's |
|---|---|---|
| **Code** | fixed programs shipped in the firmware, about 8000 instructions in the one that runs: a dispatcher, a handler per event and a scheduler | a small runtime, and each kind of job compiled into straight-line code the first time it is sent, then kept; a full code space idles the GPU and starts over |
| **What starts it** | completions, the first kick register for every command, and a 1 ms timer | completions and the second kick register only, sent when the GPU has run out of jobs or the binning stream has while shading waits; no timer, so a lost event stops the queue where the stop's readout shows it |
| **The host's requests** | a ring of 256 typed commands: a render, a 2D operation, power, context suspend, cleanup, information, queue processing | two rings of jobs, binning and shading, each job an entry in compiled code and a data slot with the values that change from send to send |
| **Register writes** | each value loaded from the command or a control block in memory first: about 11 instructions and 1.5 data fences a write, about 80 writes a binning kick and as many a shading kick | immediate stores of constants; only the values that change come from the job's data slot ([microkernel.md](microkernel.md)) |
| **Scheduling** | an event posts work bits and starts a task per unit; a common tail looks at every unit's queue and posts what is ready | one run at a time, in rounds over the two streams: a shading job that waits for its binning, or a binning job for parameter pages, stores where it stood and ends the run; the next event goes on from there |
| **Completion** | each render's status words stored to memory, then the CPU interrupted, after every binning run and every render | every job counts itself done; the CPU is interrupted only when a count it armed is passed |
| **Page tables** | written by the CPU when an application maps memory; the microkernel only points the MMU at a directory | page-table stores and an MMU invalidate between jobs, so a guest page can move while queued work still reads its old copy |
| **Watchdog** | the 1 ms timer compares, every 100 ticks, a signature of each unit's progress registers, and the kernel samples the shader cores' event service counts every 67 ms; a lockup is dumped and classified, and on a retail console the GPU is restarted for the classes the driver recognises | none on the GPU: a poll that runs out stops the queue and records where it stood, the CPU dumps the GPU's state and the emulator restarts as after a crash. A hang's cause is removed instead |
| **Power** | a power command stores the microkernel's state and idles it; the start sequence brings it back | the queue is drained and the GPU handed back to Sony's driver; after the resume the runtime starts again with its counts where they stood |

Shared by both, because the hardware leaves no other way:
- **Privileged registers through a supervisor routine.** The shader cores' code bases, the soft
  reset and the MMU's directory banks take a write from an event task only in supervisor mode, so
  both write them through a vector programmed for core 0. sgxd's passes the register and value
  through memory, since the vector does not see the calling task's secondary attributes.
- **Binning over shading.** Both start the next binning run while a render is still shading,
  touching none of the shading side's registers.
- **The cache pattern.** Both flush a unit's lines before reporting its work done, invalidate in
  the same places around the binning and shading runs, and turn the 2D unit's clock on for each
  operation and back to automatic at its completion.

## Hardware settings that differ

- **Binning split.** The four cores share a binning run in runs of primitives, in one of two split
  modes. Sony's driver keeps the first and, on a retail console, restarts the GPU after a clipper
  lockup; sgxd sets the second, under which a frame that locked a core with the first bins clean
  ([ta.md](ta.md)).
- **Clipping.** sgxd puts each draw's vertices behind the eye past the near plane, never uses
  user clip planes and widens the guard band as far as the stored coordinates allow, all to keep
  the clipper away from its lockup ([ta.md](ta.md)).
- **Parameter memory.** Both keep one buffer for every render. When binning runs out of pages,
  Sony's driver raises the limit into a reserve and restarts the binning; sgxd's buffer is far
  larger than any render needs, and a binning run starts beside unshaded scenes only when it is
  sure of its pages, so it never runs out ([ta.md](ta.md)).
- **No appending to a binned render.** libgxm's mid-scene flush bins more of a scene into a render
  already binned. A binning run on a core the scene already used replaces that core's part of it, so
  a scene takes at most four binning runs, one on each core; sgxd spreads every binning run over all
  four cores, so it never appends.

## What sgxd gives up for it

No recovery, no other process drawing while it holds the GPU (system overlays and dialogs wait),
no Razor (Sony's GPU profiler), and a dependence on driver builds whose layout sgxd knows.
