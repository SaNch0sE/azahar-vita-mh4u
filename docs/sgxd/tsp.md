# Iterators and the texture unit (TSP)

What feeds a pixel program: the varyings, interpolated across each primitive, and the texels.

## Iterators

- **Issued by the data mover.** A pixel program's varyings arrive in its primary attributes, one
  issue each ([pds.md](pds.md)), as 32-bit or 16-bit floats, or a colour as four bytes
  ([usse.md](usse.md)).
- **Perspective-correct or screen-linear,** chosen per issue.
- **Precision.** The iterator does not deliver full floats: each value has a 16-bit significand.
  It sets up a plane per 32-pixel tile, relative to the tile's corner, so the rounding error is
  constant across a tile and changes at tile edges.
- **The pixel's position** is an iterator too: its x and y at the pixel centre, its depth, and in
  the fourth lane 1/w.
- **No flat varyings.** The hardware has per-vertex flat shading in its issue words, but Sony's
  compiler never asks for it, and neither does sgxd: every varying is interpolated.

## Texture reads

- **Issued before the program,** by the data mover, when the read is a plain read at an
  unmodified varying: the texel is waiting when the program starts.
- **Dependent reads,** inside the program: coordinates from registers, the texture's state words
  from the secondary attributes, the result after a wait ([usse.md](usse.md)).
- **Tables in place of dependent reads.** A function of one varying that an address mode
  reproduces is made into a 1024x1 texture and issued like a plain read, filtered between its
  texels. The fog factor over depth is one. The PICA's procedural texture is another when it
  depends on u alone, without noise, shift or levels: mirror-clamp addressing reads it at
  min(|u|, 1), mirror at the mirrored repeat. The table holds the colour map and the colour table
  already applied, in 8 bits a channel, and is made again when the game changes either.
- **State:** four words per texture: format, layout, size, address, filters, address modes, the
  channel swizzle, the mip clamp and level-of-detail bias, and whether fixed-point channels are
  normalised (off, they return their integer values).

## Layouts

| Layout | Texel order |
|---|---|
| Swizzled | Morton order over the whole texture, y in the low bit. Not square: Morton over the smaller side, the larger side's remaining bits above |
| Tiled | 32x32 tiles, row-major inside a tile and between tiles |
| Linear | rows of the width rounded up to 8 texels |
| Linear strided | rows at a given stride |
| Cube | six swizzled faces back to back; with mipmaps each face is its whole chain to 1x1, its slot rounded up to 2 KiB; a face's (s, t) as OpenGL's. ETC1 and 8-bit faces the same way (an ETC1 face's blocks in swizzled order), measured from 16 texels a side without mipmaps and from 64 with them |

- Each axis spans its size rounded up to a power of two: texels from the size up to that power
  read the last column or row, and repeat and mirror wrap over the rounded size.
- Mip levels follow each other with no gap.
- **None of them is the PICA's.** The PICA stores 8x8 tiles with x in the low bit of their Morton
  order, which no format or layout here reads. So every PICA texture gets a copy the texture unit
  can read, made on the GPU, except one with a side under 8, which the CPU decodes. The trick that
  makes it cheap: read as a swizzled texture 8 texels wide, the PICA's tiled bytes are its tiles
  stacked, each one transposed, so a copy is a draw of plain quads with transposed, point-sampled
  coordinates.

## Formats

Everything the PICA needs, read directly or with a channel swizzle: 8888, 888, 565, 5551, 4444,
88, 8, 4-bit through small palettes, 16-bit floats.

- **Swizzles** depend on the format's channel count: one-channel formats can spread their channel
  to any lane, two-channel ones swap or spread, three-channel ones swap red and blue, four-channel
  ones swap red and blue, reverse the channels, and force alpha to one.
- **ETC1** is decoded by the hardware, but each 8-byte block's two words are read in the other order
  from the PICA's, and the blocks in the texture unit's own order: a copy moves and swaps the
  blocks without decoding them. ETC1 with a 4-bit alpha has no format of its own: its colour goes
  as ETC1 and its alpha as an 8-bit texture beside it, a cube's as an 8-bit cube read at the same
  direction.
- **Palettes** of 4 and 8 bits look up 32-bit entries, the low nibble first, as the PICA's 4-bit
  formats are laid out.
- **YUV** formats convert with BT.601 or BT.709 coefficients in fixed point.
- **Borders.** The border address modes expect a block of border texels before the image, with no
  single border colour. sgxd gives the PICA's border colour in the pixel program instead.

## Filtering

- **Bilinear weights are 6 bits, truncated.** The fraction of a texel coordinate is cut to 1/64 in
  both directions.
- An unconverted read of an 8-bit four-channel texel is filtered byte by byte, rounding to nearest
  after each direction.
- Half floats filter linearly; 32-bit floats do not.
- No anisotropic filtering.

## Reads that fault the GPU

Sampling any of these takes the GPU down:
- gamma conversion on the 64-bit formats and on the 32-bit formats with 9- to 11-bit fields;
- the reserved format codes (one decodes as a 24-bit format, every other one faults);
- a 4-bit palette read right after a YUV 4:2:2 read in the same render.
