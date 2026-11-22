# sgxd: driving the SGX ourselves

azahar does not draw through libgxm. It takes the GPU, an SGX543MP4+, from Sony's driver and drives
it through a layer of its own, sgxd.

sgxd has three parts:
- a **kernel part** in the emulator's kernel plugin. It takes the GPU from Sony's driver and
  gives it back, maps memory for the GPU, runs register lists and owns the GPU's interrupt;
- a **user library** that builds what the GPU runs: renders, the small programs that feed each
  shader, and fills and copies;
- a **microkernel**: our own code running on the GPU, which works through the queue of jobs so
  the CPU does not have to.

## A tiler pretending to be an immediate-mode renderer

The PICA200's render target is a register, and games switch it every few draws to build a frame
through a chain of small buffers. On the SGX543, a tile-based deferred renderer, a switch ends a
render, which stores every tile to memory. So the renderer collects draws into **passes**, one
GPU render each, and keeps a pass open as long as it can. On sgxd:
- draws read guest vertices and 16-bit indices where they lie;
- at the 3DS's own resolution a pass stores straight into the guest's buffer in the PICA's 8x8
  tiling. Upscaled, with widescreen margins or multisampled, it stores into the colour buffer's
  *picture*, a surface of its own in the guest's pixel format at the scaled size, which the GPU
  writes into the guest's buffer only when something else reads those bytes; a buffer the
  pictures do not take is stored straight into guest memory. While a picture is newer than the
  guest's bytes, the pages under it are closed to the guest. A game that keeps its own data on a
  page it shares with a screen's output would trap at every write, so such an output is written
  into guest memory at each transfer instead ([pbe.md](pbe.md));
- textures get a copy the texture unit reads, made on the GPU ([tsp.md](tsp.md)); only those with
  a side under 8 are decoded on the CPU, where a GPU render would cost more than the decode;
- fills and texture copies run on the GPU's 2D unit ([ptla.md](ptla.md)), display transfers as
  small renders;
- the PICA's shaders and its fixed-function fragment pipeline become machine code for the SGX's
  shader cores, written by the emulator itself ([shaders.md](shaders.md)).

Every PICA interrupt goes to the guest when its work is submitted, not when the GPU has done it, so
the emulation and the GPU run in parallel. The guest pages queued work still uses are held against
the guest until that work is done, and a page the guest rewrites while the GPU reads it gets a
second physical page ([memory.md](memory.md), [../native-execution.md](../native-execution.md)).
When the GPU falls behind, a frame leaves out the busier screen's draws rather than slowing the
game (the late skip, [frames.md](frames.md)).

There is no CPU fallback: a case the GPU path does not handle is an assert naming it.

## The GPU, unit by unit

Four identical cores, each with every unit of the pipeline, behind a master that holds what they
share: the top of the memory interface, the system-level cache, the 2D unit, and the event
registers.

| Unit | What it does | File |
|---|---|---|
| Memory interface and MMU, system-level cache | GPU addresses, page tables, faults; the cache in front of memory | [memory.md](memory.md) |
| Data masters and data mover (PDS) | start every task on the shader cores and load its inputs | [pds.md](pds.md) |
| Shader cores (USSE) | run the vertex and pixel programs, and the microkernel | [usse.md](usse.md) |
| Tile accelerator (TA) and parameter memory (DPM) | clips, culls and projects vertices, sorts primitives into tiles, writes the parameter buffer | [ta.md](ta.md) |
| Image synthesis (ISP) | per tile: hidden surface removal, depth and stencil | [isp.md](isp.md) |
| Iterators and texture unit (TSP) | the varyings and the texture reads of pixel programs | [tsp.md](tsp.md) |
| Pixel back end (PBE) | writes each finished tile to memory | [pbe.md](pbe.md) |
| 2D unit (PTLA) | fills and copies | [ptla.md](ptla.md) |
| Events | completions, kicks and the interrupt, which the microkernel runs on | [microkernel.md](microkernel.md) |

The programs: [shaders.md](shaders.md). Frames, the late skip and pacing: [frames.md](frames.md).
Taking the GPU from Sony's driver and living beside the system: [takeover.md](takeover.md). Where
sgxd differs from Sony's driver and microkernel: [sony.md](sony.md).

## One render

A render is two runs over one target:

1. **Binning** (the TA). The vertex data master walks the draws' index lists. For each batch of
   vertices the data mover starts a vertex program on a shader core. The finished vertices go to
   the tile accelerator, which clips, culls and projects them, sorts each primitive into the
   32x32-pixel tiles it touches, and writes a list per tile into the parameter buffer.
2. **Shading** (the 3D). For each tile with something in it, the ISP takes the tile's primitives
   and decides visibility per sample, on chip. The pixel data master starts the pixel programs,
   fed by the varyings and texture reads the data mover issues, and their colours stay in the
   tile's on-chip storage. When the tile is done, an end-of-tile program tells the pixel back end
   where and how to write it to memory.

The four cores share both runs: binning in runs of primitives, shading by tile. A tile that no
primitive reaches is skipped entirely: it gets no background, no load and no store, and its memory
keeps its old bytes.

Everything one render needs (the tile layout, the end-of-tile program, the room for the draw
stream) is prepared once per target size and kind, so opening a render on a target of a known size
costs no setup. Every render is a job for the microkernel.

A game that draws its two eyes into two buffers in turn, a draw or two each, would end a pass
at every switch. Two passes can stay open together instead: the one
left is put aside unsent and taken up again at the next switch, and both are sent together when
anything else ends them. That needs buffers that do not overlap, so that the two renders give the
same result in either order. A pass's data (its vertices' setup, its programs' constants) lives in a
ring that a later pass reuses only once the GPU is done with it; two passes sent together are
released together, since their draws share data.

## Why not libgxm

1. **The game's buffers are in the PICA's 8x8 tiling, and the game reads them back.** libgxm
   stores a render in one of three layouts of its own, with one store description for every tile;
   sgxd's end of tile writes the PICA's tiles into the game's buffer, on the GPU.
2. **A frame is many small renders**, a dozen or more. A libgxm scene took about 500 µs
   whatever its size; sgxd prepares a render once per target size, and a render then costs
   its work (about 30 µs for a 32x32 one).
3. **The game rewrites memory that queued work still reads.** libgxm maps memory for all queued
   work at once and reports progress per job; sgxd's microkernel moves a guest page between jobs,
   in queue order, and nothing waits.
4. **Geometry shaders.** Neither the SGX nor libgxm has the stage, and libgxm lets a program store
   to memory only through a buffer declared in a compiled program; sgxd runs the stage as
   stream-out renders queued ahead of the pass.
5. **Programs made while the game runs.** libgxm takes programs only from Sony's compiler, which
   on the console is slow and has to be extracted by the user; the emulator writes USSE machine
   code itself.
6. **Operations on the target's pixels.** libgxm's blend has no constant factor and no logic
   operations, and only a compiled program reads the target's pixel; sgxd's programs do the PICA's
   blending themselves, its shadow writes included.
7. **The clipper lockup.** sgxd keeps each draw's vertices behind the eye past the near plane and
   bins in the split mode that avoids the lockup; libgxm has no say in either.

What owning the GPU costs: [sony.md](sony.md#what-sgxd-gives-up-for-it).

## Rules that keep the GPU running

Breaking any of these hangs the GPU, faults it, or silently loses work:

- Never rewrite a data-mover program at an address that has run. Reset the data mover only
  together with the shader cores ([pds.md](pds.md)).
- New shader code at an address that has run needs the code cache invalidated
  ([usse.md](usse.md)).
- Never invalidate the system-level cache while work is storing through it
  ([memory.md](memory.md)).
- The clipper can lock up the binning unit: each draw's vertices behind the eye past the near
  plane, no user clip planes, a wide guard band, and the second binning split mode
  ([ta.md](ta.md)).
- A binning run that runs out of parameter pages while other scenes wait for their shading stops
  the GPU for good ([ta.md](ta.md)).
- A program that discards pixels must be drawn as punch-through, and discard in its own first
  phase ([isp.md](isp.md), [usse.md](usse.md)).
- The tile's last emit must advance the pixel back end, and averaging emits mixed with full-size
  ones need a fence before them and a real clip ([pbe.md](pbe.md)).
- A twiddled store's base lies on a multiple of 128 bytes ([pbe.md](pbe.md)).
- An upscaled render must not keep its draws inside part of the target with the mask
  ([isp.md](isp.md)).
- A pixel program needs an issue that goes to the shader core: texture issues alone never start
  it ([pds.md](pds.md)).
- Gamma on some texture formats, the reserved format codes, and a palette read right after a YUV
  read in the same render all fault the GPU ([tsp.md](tsp.md)).
- Privileged registers take effect only through the supervisor routine, and the first job after
  the takeover must store something before its first store fence
  ([microkernel.md](microkernel.md)).

## Accuracy against the 3DS

- vertices snap to 1/16 pixel;
- bilinear filtering uses 6-bit weights, truncated;
- the pixel back end's and the 2D unit's format conversions truncate;
- iterated varyings have a 16-bit significand, set up per 32-pixel tile.
