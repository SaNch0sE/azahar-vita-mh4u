# The 2D unit (PTLA)

A unit on the master, beside the four cores: fills, copies and blits between surfaces in memory,
with format conversion and a 2x2 downscale. The PICA's memory fills and texture copies run on it,
in place at the guest's addresses.

## Driving it

- **A command stream,** written word by word into one register of the master: a destination
  surface, perhaps a source surface, the operation and its rectangle, and a flush that ends the
  stream and starts the work. One stream can hold many blits.
- **Completion** latches an event on the master, which goes to the microkernel or to the CPU's
  interrupt ([microkernel.md](microkernel.md)).
- **Its own page directory slot** in the MMU, which sgxd points at the directory every requestor
  shares ([memory.md](memory.md)).
- **Its clock** has to be on while it works; Sony's firmware turns it on for each operation and
  back to automatic gating at the completion, and sgxd does the same.

## What it does

- **Layouts:** linear, 32x32 tiled, and twiddled. With an 8-pixel stride, its twiddled layout is a
  row of PICA tiles, so it can decode the PICA's tiles and encode them back, exactly; that is no
  faster than the pixel back end's strips ([pbe.md](pbe.md)).
- **Formats:** 8888, 565, 4444, 1555, 2101010 and raw words; the 8-, 16- and 24-bit integer formats
  store the low bytes of each word. Conversions truncate. A fill takes its colour as an 8888 word
  and converts it, except into 2101010, where the word is written raw.
- **Bytes:** an 8-bit copy moves bytes exactly, from and to any byte, its start coordinate giving
  the byte offset within a word. A guest DMA from or to an address no word starts at is such a
  copy; a word-aligned one copies whole words and leaves its last few bytes to the CPU.
- **Overlap.** A blit reads its whole source before it writes, and the copy order follows the corner
  the start coordinates name; an overlap is safe within one blit, not between two.
- **Edges** wrap at 13 bits.
- **Through the system-level cache,** like the units whose output it copies. Sony's firmware makes
  no cache request around a 2D operation, and neither does sgxd.

## Ordering with the 3D

A 2D operation on memory that queued shading still uses waits for that shading, and the next
render's binning waits behind the operation: the 3D side runs dry meanwhile. So a fill is no free
substitute for a render: a clear that later draws join costs less as the render's first draw.

The PICA's display transfer reads PICA tiles with scaling and format conversion, which this unit
cannot do in one operation; it is a small render instead.
