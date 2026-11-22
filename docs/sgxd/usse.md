# The shader cores (USSE)

Each of the four cores has a USSE, a programmable unit that runs every vertex and pixel program,
and on core 0 the microkernel too. The emulator writes its programs as USSE machine code.

## Tasks and pipes

A USSE has four pipes. Each schedules its own tasks: up to 16 slots, a few active at a time, taken
round robin, and a task that waits (for a load, a texture read, a branch condition) gives way to
another. A pixel task covers 2x2 blocks of pixels, one *instance* per pixel; the data mover sizes
the tasks ([pds.md](pds.md)). During shading, a 32x32 tile's four 16x16 quadrants run on the four
pipes.

## Registers

- **The unified store**, 3328 registers a core, is where tasks take their temporaries and primary
  attributes from, and where the output partitions live. How many temporaries a program declares
  decides how many of its tasks fit at once (below).
- **Primary attributes** are a task's inputs: vertex attributes, iterated varyings, texels, as
  the data mover writes them.
- **Secondary attributes** are per pipe, not per task: constants a secondary program loads. A
  draw's secondary program runs for each pipe that shades it, and what a task writes there stays
  on that pipe until the next secondary load.
- **Outputs.** A vertex program's outputs go to the tile accelerator. A pixel program's output
  registers are the tile's colour on chip: they hold the pixel's current colour when the program
  starts, and what is in them at the end is the pixel's new colour. Blending and colour masks
  read the colour there, with no memory traffic.
- **Internal registers**, four of them, of four 32-bit lanes each, are the only operands a
  four-wide full-float operation reads and writes whole. They do not
  survive anything that deschedules the instance: a wait, a branch, a label, a phase boundary,
  and the boundary after the instruction that follows a dual-issue word (below). A three-wide
  dot product into one writes no lane w (a four-wide one does, and so does a reciprocal square
  root).
- **Special registers** give a task its pipe, its core and, at the end of a tile, its block of the
  tile ([pbe.md](pbe.md)).

## What the instructions do that a reader would not expect

- **Full-float vector operations on the unified store work a pair at a time.** A four-wide F32
  operation whose destination is an ordinary register writes its first two lanes only; writes
  aimed at the third or fourth lane are silently dropped, and a second source's third and fourth
  lanes are not read either. The third and fourth lanes are the next register pair.
  Half-float values pack four lanes into two words.
- **Constants in any lane.** A vector operation's first source can take 0, 1, 2 or one half in
  any lane, in place of that lane's register word.
- **The integer colour unit** works on four 8-bit lanes: each product is x·y/255 rounded to
  nearest, a sum of two products rounds once, results saturate, and a complemented factor is
  255 − x. That rounding is exactly the PICA's, which lets the PICA's colour combiners, fog and
  blending run in bytes, byte-exact.
- **Packs to bytes** multiply by 255, round to nearest and saturate (NaN becomes 0). Without the
  scale flag a pack converts two lanes and writes them twice. Packs to half floats saturate at the
  largest finite half.
- **16-bit integer operations** work on two lanes with no carry between them, and a test's
  predicate follows the low lane. A full 32x32 multiply-add takes two instructions. Shifts by 32
  or more give 0. Some forms sign-extend a 7-bit immediate.
- **Tests:** zero counts as positive.
- **Dual-issue words** hold two operations, and both read their sources before either writes.
  One result goes to an internal register, the other to the unified store (a pair, or one
  register for a scalar result) or to a whole internal register; between them they read one
  unified-store operand, and the other sources are internal registers. Such a word costs what
  one word costs. **It has no no-schedule bit, and pixel tasks do not take it as set**: the
  boundary after the instruction that follows it is unguarded, and an internal register live
  across it is lost when the pipe runs another task there. So operations pair only where nothing
  internal is live past that point. Run alone (one task, as bare-metal tests run) the same words
  are right.
- **Exponent and logarithm** are base 2.
- **Rounding** in packs to 16-bit integers is to nearest only.

## What the programs read as it is

Moving values between forms is a large share of a lit pixel program's words. The hardware takes
these without a move:

- **The vertex colour as bytes.** A colour iterator without its float form delivers the four
  bytes in one register, the same bytes a pack of the four floats gives.
- **A texture read first.** A program need not take the vertex colour first: its first issue can
  be a texture read, which the data mover writes first.
- **Byte textures read raw.** A texture stored as four bytes in their channels' order is read as
  its stored word, filtered bytewise. A compressed texture read so gives the same bytes but costs
  more than its half-float read.
- **Constant colours in place.** The integer colour unit reads constant registers below 64
  directly; only the three-source form's first operand cannot be a constant register.
- **Half floats where they save words.** A half-float word costs what a float word costs, but
  four half lanes fit one register pair: directional, diffuse-only lighting runs in half floats
  with no moves into internal registers (11-bit vectors: an approximation that changes few
  pixels).
- **Work after the last read.** A wait drops the internal registers, so sums and terms made after
  a program's last table read stay in internal registers, and every light's vectors made before
  the first read need no store and reload around the reads.
- **Operand limits.** A test's second source cannot be negated, and a vector operation's second
  source reads no third or fourth lane (above): two-wide operations on stored pairs, and values
  stored negated, avoid the copies.

## Memory and texture reads in a program

- A load from memory or from a register, and a texture read issued by the program, return their
  data only after a wait on the read's counter. Several reads may share one wait, and the
  internal registers do not survive it.
- A texture read's result lands in an aligned register pair; a 32-bit or 16-bit float result
  starts on a 64-bit boundary.
- A load of more than four words needs that many consecutive registers inside the program's
  declared counts. A load that runs past them writes registers the program does not own.
- A store fence (issue, then wait) makes the core wait until the memory system has accepted the
  stores before it: accepted by the system-level cache, not yet in memory
  ([memory.md](memory.md)).

## Scheduling bits

- **No-schedule** keeps an instance running across the next instruction boundary. Sony's compiler
  sets it exactly where an internal register is live across that boundary, and so does sgxd.
  Misplaced, it shows as sparse noise in single pixels of textured and lit draws.
- **Skip-invalid** skips the instances of a task whose pixels are not covered. It is cheap to set
  and costly to leave clear on blended and overdrawn draws. It must be clear only where a texture
  read computes its level of detail from its 2x2 neighbours: on that read and on what makes its
  coordinates. A read that is point sampled from a one-level texture (a lighting table, a shadow
  tap, a procedural texture's tables) needs no neighbour and takes it.

## Phases and discarding pixels

A program can run in phases: a phase hands over to its successor, and an end flag inside a first
phase ends that phase, not the program. A program that discards pixels (the PICA's alpha test)
does so in a first phase of its own, with one kill; the kill's pass condition also runs the ISP's
alpha comparison against a feedback word in the secondary attributes. Programs with three phases,
or a second kill inside the second phase, lock the GPU. How the ISP has to treat such a program:
[isp.md](isp.md).

The first phase runs for every pixel the ISP hands the program, so what stands before the kill
is paid for the pixels it drops too; registers written there are still the program's in the
second phase. A lit program therefore tests its alpha first wherever the tested alpha comes
from texels, the vertex colour and constants alone: the first phase is those texels, the alpha
of the combiner stages it comes from, and the test; the shadow, the lights and the whole
combiner follow the kill, and the pixels it drops pay only for the first phase.

## Code

- Program addresses are instruction indices from one of sixteen code base registers. Each draw's
  programs name a base; the microkernel keeps two for itself. Writing a code base is privileged
  ([microkernel.md](microkernel.md)).
- **The code cache.** Each core caches instructions by address, and a line that has run keeps
  running its old code after a rewrite until the code cache is invalidated. sgxd places each
  program's code once and invalidates the code caches at every render's kick; new code placed a
  cache line and a prefetch away from all code that has run would need no invalidate at all.

## What a program costs

Measured:

- **Instructions times pixels.** About a third of a nanosecond a pixel-instruction across the
  four cores in heavy programs, up to about twice that in short ones.
- **Latency is hidden only with tasks in flight.** A texture read the data mover issues before the
  program starts costs almost nothing. A read inside the program, and anything waiting on one, is
  hidden only while other tasks can run.
- **Declared temporaries limit the tasks in flight.** On a long dependent chain, 40 declared
  temporaries ran at a quarter of the rate of 4. An opaque layer hardly notices; stacked blended
  layers and programs full of dependent reads slow down a lot.
- **Constants in registers cost nothing; past the registers they are loaded at every pixel.** A
  pixel program has 128 registers for its constants. A lit pixel's lights take four for each
  colour or vector their program reads, so a zero colour and a direction nothing reads take
  none, and their terms are left out.
- **No instruction cache limit** was found, even with many different programs interleaved.
- **One task alone** (the microkernel's case) takes about 0.2 µs an instruction of any kind, about
  1 µs to read a register with its wait, and about 2.4 µs to load from memory.
