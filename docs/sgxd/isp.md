# Hidden surface removal, depth and stencil (ISP)

The first unit of the shading run. For each tile it takes the tile's primitives from the parameter
buffer and decides, per sample, which of them is seen, before any pixel program runs.

## What it keeps on chip

Per tile and per sample: a 32-bit float depth, an 8-bit stencil and a one-bit mask. The tests
run in fixed function, and no program can read these values. The only way out is to store the
tile's depth to memory and read that back as a texture in a later render.

## Object types

Each draw's state gives its primitives a type, and the type decides when the ISP can resolve
visibility:

- **Opaque:** hidden surface removal. Of all the opaque primitives over a sample, only the one in
  front is shaded, whatever the draw order.
- **Translucent:** shaded in draw order over what is under it. Its program reads the pixel's
  current colour on chip ([usse.md](usse.md)), so blending and colour masks cost no memory
  traffic.
- **Punch-through:** the pixel program decides whether a pixel survives (an alpha test). The ISP
  waits for the program's answer before it can reject anything behind.
- **Depth feedback:** the program writes the depth. Nothing is rejected before the program has
  run, and every layer of overdraw is shaded.

A program that discards pixels must be drawn as punch-through. Its first phase waits for the ISP's
feedback, which the opaque and translucent types never give: under them, every pixel is lost.
Punch-through and depth feedback both make the ISP wait, so the emulator avoids them where it can:
W-buffered depth comes from the tile accelerator rather than from the program
([ta.md](ta.md)), and an alpha test whose failing pixels the blend would leave unchanged anyway
(and that writes no depth or stencil) gives them alpha 0 in a translucent draw instead of
killing them.

A draw that blends nothing but keeps one channel (the alpha, with its write switched off) reads that
channel from the pixel under it, so it seems to need the translucent type, which would lose hidden
surface removal for every layer it draws. It can stay opaque as long as no opaque primitive that
writes the channel is still waiting, unshaded, under it: hidden surface removal would drop that one
and its value would never be written. A translucent object over the whole render whose program
leaves the pixel as it is settles that: every tile shades what is waiting before it. With one such
object between the draws that write the channel and those that keep it, the latter stay opaque: a
scene of lit meshes drawn this way took about a sixth less time opaque than translucent.

## The background

There is no hardware clear. Each tile starts with a background object, a program that writes the
tile's starting colour, and the tile's depth and stencil start from the render's background
values or from a load. A tile no primitive reaches does not even run that: it is skipped whole and
keeps its memory.

## Depth in memory

The ISP loads and stores depth and stencil per tile, in its own format, only for the tiles a render
touches. sgxd keeps depth in stores of its own: a render stores its depth there, and the next render
on that buffer loads it. A render that needs the guest's own depth bytes converts them into a store
first; an upscaled, widescreen or 2x render takes them magnified, each sample the word of the pixel
it lies in and the widescreen margins at the far plane. A render that leaves part of the buffer
undrawn (another buffer's colour over its first rows) then gives those bytes back unchanged when
they are written back, since a pixel's centre sample holds its own word. Depth goes back to guest
memory only when the guest reads it, by a transfer, a DMA or a texture: a render of its own has the
ISP write the store again in the guest buffer's format (24-bit depth with the stencil byte above it,
or 16-bit depth, both of which the ISP stores itself), and a copy writes those values into the
guest's tiles, only the tiles the read covers. An upscaled or widescreen store gives each pixel the
sample its centre falls in. 24-bit depth without a stencil byte goes back as its three depth bytes.
Depth the TA wrote as W goes back whole, and the CPU then turns it into the guest's depth by the
line fitted to the draws' projection; W depth in a 16-bit buffer, or with no such fit, stops the
emulator.

Until it goes back, the store is the newer copy of the buffer and guest memory the older one. A
write of the guest's own to the buffer's bytes ends that: a buffer the game has freed is ordinary
memory again, and a later read from it must not bring the old depth back over what the game put
there. So the whole pages of a depth buffer in main memory are watched
for the guest's writes, as a texture's pages are, and a write there makes the store the older copy
at that point in the queue. The buffer's ends lie on pages it shares with other data, where a
write may be the neighbour's: their bytes are kept each time a render stores the depth and
compared before a write-back, and other bytes there mean the guest wrote them. This is not exact
for a guest that writes part of a depth buffer and reads the rest back without drawing between:
the rest is then what memory held, not the depth. Depth buffers in video memory are not watched;
the GPU's own writes over them (fills, copies, another buffer's colour) already end a store.

## Clipping and the mask

- **Region clip:** per draw, a window in whole tiles, inside or outside of which nothing is drawn.
- **Stencil and the mask:** a draw's stencil test can set or clear the per-sample mask, and a
  sample whose mask is clear rejects every later object but the background. sgxd uses it to keep
  the draws inside a viewport smaller than the target: the mask is cleared outside the viewport
  once the render is open, and the draws stay opaque, which a kill at the viewport's edges would
  not allow.
- **A viewport on the store grid** (the steps a part of the target can be stored in: columns in
  8s, rows in 8s from a multiple of 16) **is masked too**, though only its part of the target is
  stored. The SGX draws a triangle that crosses the viewport's edge out to the target's edge, and
  every fragment there is shaded for nothing: a scene drawn into 400x240 of a 512x256 target shaded
  a fifth of its fragments in the margin, and took a quarter less time masked. The draws also
  take the region clip of the tiles the viewport touches, and the mask covers the margin inside
  those tiles only: a tile with anything in it is rendered, loaded and stored, so a mask over
  the whole margin costs the tiles a render would otherwise skip.
- A render masked from its first draw writes no depth past its part either, so its depth goes
  straight into the kept depth: the margin there keeps what the render loaded, or the clear.
- In an upscaled render, keeping the draws inside part of the target with the mask hangs the GPU
  within minutes, at every scale tried. Such renders take no mask on the store grid, draw their
  depth into a scratch buffer and copy the part back instead.

## Multisampling

With 2x multisampling (the top screen's option) a pixel has two samples, one above the other, and
the ISP tests depth and stencil at each: a tile is still 32x32 samples, so it holds 32x16 pixels,
and the region arrays and the macrotiles' rows count twice the tiles. A pixel program still runs
once a pixel, its colour written to the samples it covers, and the tile's store averages them. Two
things follow:

- **Depth in memory is the samples'**. A store holds both samples of every pixel, twice the rows,
  and only a 2x render can load it back; a guest read of it takes one sample a pixel. Depth that
  stays within one render costs nothing more; a store kept for the next render on the buffer costs
  twice the memory. A 1x store is not loaded into a 2x render, nor the other way: a buffer that
  turns 2x loads the guest's depth once, each pixel's word in both its samples, as one that turns
  upscaled loads it magnified.
- **The picture is stored linear, then copied.** The emit that averages the samples does not
  produce the PICA's tiles the way the strips of a 1x store do, so a 2x pass averages its samples
  into the buffer's picture, at 1x, and the copy of that into the guest's buffer, made when
  something reads those bytes, holds the smoothed picture too.

Multisampling does not combine with upscaling: while it is on, every buffer is drawn at the 3DS's
resolution.
