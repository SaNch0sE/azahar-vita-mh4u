# The data masters and the data mover (PDS)

Nothing runs on the shader cores without the data mover. The cores have no reset vector, and a
task never starts itself: every vertex, pixel and microkernel task is started by a short program
for the PDS (the programmable data sequencer), in an instruction set of its own.

## Three data masters

Work reaches a core's data mover through three data masters:

- **Vertex:** walks a render's draws: each draw's state, then its index list. For each batch of
  indices it runs the draw's vertex data-mover program.
- **Pixel:** during shading, per tile and primitive, it runs the draw's pixel data-mover programs
  for the pixels the ISP keeps.
- **Event:** hardware events and kicks from the CPU run an event program, which starts the
  microkernel ([microkernel.md](microkernel.md)).

## What a data-mover program does

Its outputs:
- **start a shader task:** the program's address (an index from one of the cores' code bases),
  its temporary register count and its execution mode;
- **write primary attributes**, the task's own input registers, directly;
- **DMA memory into primary attributes**, from any address;
- for pixels, **issue the iterators and texture reads** whose results the task finds in its
  primary attributes ([tsp.md](tsp.md)).

## The programs sgxd builds

- **Vertex fetch.** Per attribute, a DMA of `address + index × stride`. The DMA reads from any
  byte address at any stride, so the guest's vertex buffers are read where they lie, however the
  guest packed them. Index lists take 16-bit indices at any even address; the PICA's 8-bit indices
  are widened.
- **Constants through secondary programs.** A draw's uniforms go into the secondary attributes
  (per-pipe registers, [usse.md](usse.md)) through a secondary data-mover program that DMAs the
  draw's uniform image. Two other ways fail on the hardware:
  - a pixel primary program's own attribute writes reach only one task per primitive and tile:
    every other pixel reads stale registers;
  - loads that a secondary shader program makes itself may not have landed when a draw's first
    vertices read them.
- **Pixel inputs.** Start the task, then one issue per iterator or texture read. The issues fill
  the primary attributes in their order, and within one issue the iterator comes before its texel.
  A texture read issued this way needs the task marked as depending on it, or the program runs
  before the texel lands and reads stale registers. A task starts only at an issue that goes to
  the shader core: a program fed by texture issues alone never runs, and the render never finishes.
- **Task sizes.** Each draw carries words that tell the data masters how many registers one vertex
  or pixel takes and how many 2x2 blocks one task holds. The hardware allocates register storage by
  them, so they must be right; sgxd computes them as libgxm does, from the program's attribute and
  temporary counts.

## The rule for data-mover code

**The data mover keeps running a program at an address it has run**, whatever is invalidated: its
own caches, the data and code caches, the system-level cache. Only a soft reset of the data
mover clears it. And a soft reset of the data mover alone leaves the shader cores' task counters
behind, so the next binning run's first vertex task is never serviced and the run never finishes:
**the data mover is reset only together with the shader cores**, with the GPU idle.

So sgxd never writes data-mover code at an address that has run:

- The per-draw programs, which embed addresses that change from draw to draw, live in two halves
  used in turn. Each use of a half is mapped at GPU addresses of its own, so a half's memory is
  written again only under addresses it has not run at. After 64 uses the addresses run out: the
  GPU idles, and every core's data mover, shader units and caches are reset together.
- No data-mover program is kept for good: a flush of the shader code heap places the shaders
  again at new addresses, so a cache that kept every program could only grow.

The same rule is why the GPU is reset at the takeover ([takeover.md](takeover.md)): the system's
own programs are still cached at addresses sgxd is about to reuse.
