# The pixel back end and the end of tile (PBE)

When a tile is done, its colour leaves on-chip storage through the pixel back end. Nothing is
written to memory before that, and only what the end of tile asks for is written.

## The end of tile

- **An end-of-tile program** runs when the tile is shaded: one task per 16x16 block, four per
  32x32 tile, each knowing the tile's position from its inputs and its block from its special
  registers ([usse.md](usse.md)).
- **Emits.** The program writes the tile out with emits. Each emit's words give an address, a
  clip rectangle (inclusive, in absolute pixel coordinates), a layout, a pack format and its
  channel order, and a size. The back end takes one set of emit words per tile and emit (the first
  task's), so the words must be the same in all four tasks.
- **Layouts:** linear, the back end's own tiled layout, and twiddled (Morton, y first, the same
  order as the texture unit's swizzled layout).
- **Formats:** four-, three-, two- and one-byte colour formats with their channel orders,
  16-bit formats (565, 5551 and 4444 in each channel order), raw 16 bits, and two-word pixels for
  64-bit formats. Conversions truncate. The channel order says how the channels lie in memory, not
  which register lanes they come from.

Rules:
- **The tile's last emit must advance the back end's tile queue.** Every emit but the last holds
  it; a last emit that holds it too leaves the back end waiting for another, and the render never
  ends.
- **An emit reads the output registers late,** after the program has moved on: nothing may write
  them between an emit and the program's end.
- **Read the tile's position before any load and wait**: a program that waits first loses it.
- **Put a twiddled store's base on a multiple of 128 bytes.** Twiddled stores of compressed cube
  faces at bases 128, 256 and 2048 bytes into a block were right; at 184 (64-bit pixels) the faces
  read back wrong, and at 344 (bytes) the render never finished. The alignment needed, more than 8
  bytes and at most 128, is not measured.

An emit costs the back end about 1.1 µs a core, overlapped with the next tile's shading.

## Storing the PICA's tiles

The back end writes linear, its own tiled or its own twiddled layout, never the PICA's 8x8 Morton
tiles. sgxd stores PICA tiles anyway, from the end-of-tile program:

- The render draws the guest's buffer **transposed**: x and y are swapped in the vertex program,
  and the viewport and scissor are swapped to match. In that orientation a 16-row strip of the
  twiddled layout holds two PICA tiles exactly.
- The end-of-tile program computes the eight strips' addresses and clips from the tile's position,
  and has the back end write each one straight into the guest's buffer, packed in the PICA's
  colour format.
- A render whose draws only use part of the buffer stores only that part.
- **Widths a strip does not fit.** A strip pairs two PICA tiles of one row, so the store takes
  buffers 32 pixels wide or more, in steps of 16. A narrower buffer, or one an odd number of tiles
  wide (a 72x120 downscale of the top screen), is drawn into a scratch surface of the next width
  that fits: each of the guest's rows of tiles is copied to the start of the scratch's row before
  the render and back after it. Such a buffer is drawn at the 3DS's own resolution whatever the
  scale.

The result is bit-exact in all five PICA colour formats. At the 3DS's own resolution every
render ends with this store. Upscaled, with widescreen margins or multisampled, a game's renders
store into pictures instead (below); this store is then the pictures' copies into guest memory,
and the renders of a buffer that has no picture (sizes the picture path does not take, a render
narrower than its buffer).

**A light render costs its store.** The PICA's tiling takes the back end several times the emits of
a plain store, so even a render of one quad pays for storing every tile it touches; loading the
buffer first adds little. Fewer renders is what saves time.

## Pictures: colour kept until it is read

When a game is drawn other than at the 3DS's own resolution (upscaled, with widescreen margins or
multisampled), each colour buffer it draws into is drawn into a picture of its own: a linear surface
of the buffer at the scaled size, in the buffer's own pixel format (the same pack the tiled store
uses, so a 16-bit buffer's colour is cut down at each render's end, as the PICA cuts it, and a
buffer without alpha keeps none). The next render on the buffer loads its picture, a display
transfer and a texture over the buffer read the picture, and nothing goes into guest memory until
something else reads the buffer's bytes. Then a render of its own copies the rows asked for into the
guest's tiles, downscaled when the picture is upscaled.

At the 3DS's own resolution there are no pictures: a picture there holds nothing guest memory
could not, and it only puts off the store until the game samples the buffer or reads it, when the
copy comes on top.

- **One owner a row.** Each strip of eight rows has at most one picture holding its newest colour;
  a render that stores over rows another picture owns takes them over. Rows something else wrote
  since (a fill, a transfer, the CPU) are outdated in the picture: the next render that loads the
  buffer loads those rows from guest memory.
- **A screen's picture.** A display transfer from a picture makes a picture of the screen at the
  same scale, and the present shows that in place of the game's screen buffer. When the GPU needs
  the buffer's bytes (a render or a texture that reads memory the buffer shares), they are copied
  there once and the picture stays what is shown: both hold the same frame. Only the game
  touching the buffer hands the screen back to guest memory. Letting the picture go at each copy
  would make a screen alternate between its scaled, filtered picture and its buffer shown pixel
  for pixel, and every edge on it would flicker.
- **The CPU and the system's services.** Pages whose rows a picture owns are closed to the
  guest. A read or write there stops the guest, and the copy is made before it goes on.
- **Savestates.** A save ends the open render and copies every picture's rows first, so the
  state holds every draw. A load builds the GPU anew: no picture outlives it.

## Averaging emits

The back end can also average a tile's samples into a surface of the pixels' size, as it resolves
multisampling: each 2x2 block at 4x, each pixel's two samples at 2x, and each 2x2
block of an unsampled render as a downscale. sgxd uses them for its 2x multisampling alone: such a
pass stores its picture through one averaging emit a tile, the same emit libgxm's 2x renders end
with, and the guest's own copy is a render of its own, as an upscaled render's is. An upscaled
render stores its picture at the scaled size with full-size emits. Mixed with full-size emits in
one render, averaging emits have two rules:

- **An averaging emit must not be issued while full-size emits are still in the back end.** Inside
  one tile that hangs the GPU. Over a large target the same happens between tiles, because one
  core's next tile starts its averaging emits while its last tile's full-size store is still going
  out; small targets give each core a single tile and never show it. A data fence on the back end
  right before a tile's first averaging emit removes it.
- **An averaging emit with an empty clip must not be followed by full-size emits in the same
  tile.** The back end is left broken (that render or a later one hangs), fence or not. Pushing a
  strip's clip inside out is the usual way to drop a strip outside a stored part, so averaged
  strips have to keep a real clip.
