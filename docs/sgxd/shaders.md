# Shaders

Each PICA program and fragment state becomes a USSE program the emulator emits while the game runs,
in two tiers.

## What becomes a program

- **A PICA vertex program**, with the draw's shape (the inputs the loaders feed, the bool uniforms
  baked in, the outputs), becomes a vertex program. Uniforms it reads at a fixed index ride in its
  secondary attributes; those read through an address register are loaded from memory at their use
  ([pds.md](pds.md)). A program it cannot express (an unknown instruction, a jump out of a
  subroutine, loops nested deeper than two, more live registers than the unified store holds)
  stops the emulator with an assert naming it.
- **The fragment state** (the combiner stages, lighting, fog, the procedural texture, shadows, the
  alpha test and blending) becomes a pixel program. The combiners run on the integer colour unit
  where every stage has a byte form, in floats otherwise ([usse.md](usse.md)).

## Two tiers

- **The first tier** only allocates registers, with no optimising passes. The parse thread meets
  most new fragment states and vertex programs ahead of the render thread, and a worker emits them
  before the render thread reaches the draw. One still queued then is emitted by the render thread
  itself, and one a worker is emitting is waited for, the render thread emitting other queued jobs
  meanwhile.
- **The second tier** emits a program again with the optimising passes once it has drawn 100
  times, or at once when it has 256 words or more (the only way a geometry program gets there).
  Two workers run it, since one program's passes can take hundreds of milliseconds. The render
  thread installs the result before a later draw, so no draw waits for the passes.

## The disk cache

Second-tier programs are kept on the memory card, per title and per build of the emulator, and
written in batches so that the title's own reads seldom wait behind them. At a title's start a
thread reads them into memory; a draw whose program is there gets the second-tier program at once.

## Geometry shaders

A draw with a geometry shader runs as two stream-out renders before its pass, renders that store
their results instead of drawing pixels:

1. the vertex program stores each vertex's outputs to memory;
2. the geometry program runs as a vertex program, one invocation a primitive; it reads those
   outputs as its input registers or its float uniforms, as the PICA's mode puts them, and stores
   each triangle it emits.

The pass then draws the stored triangles as an ordinary draw. An invocation has room for its
share of 256 KiB, at most 1024 triangles, and triangles past it are dropped. Where the program's
code bounds what one invocation emits and the bound fits, the room is the bound, and the
stream-out draws queue up: they go out as two renders, every first stage then every second, when
the pass ends, something changes guest memory or a stage holds 64 draws. Without such a bound a
draw's stages go out at once and the pass waits for them, so the CPU can read how many triangles
each invocation stored.
