# The tile accelerator (TA) and the parameter buffer

The binning run. The tile accelerator takes the vertex programs' output, makes screen-space
primitives of it and sorts them into tiles. It has three parts: the MTE turns vertices into
primitives, the TE bins them, and the DPM hands out the memory both write.

## Vertices into primitives (MTE)

- **What a vertex carries on.** The vertex program's outputs that the draw selects: the position
  and the varyings, each varying stored as 32-bit or 16-bit floats.
- **Viewport.** Each draw has a centre and a scale for x, y and z. Screen positions are stored in
  sixteenths of a pixel, from −1024 to 3071 pixels on each axis: every vertex snaps to 1/16 pixel,
  and nothing can lie outside that range.
- **Clipping.** Against the near and far planes in OpenGL's range, −w ≤ z ≤ w (the 3DS's is
  −w ≤ z ≤ 0, so the vertex program maps one onto the other).
  x and y are not clipped at the viewport but at a guard band 1.5 times the viewport's half extent
  either side of its centre; what lies between the viewport and the band is drawn and binned like
  anything else. w is clamped to a small minimum, and user clip planes come from the vertex
  program's outputs.
- **Culling.** Degenerate primitives, primitives entirely off screen, primitives that cover no
  pixel centre, and by winding in device space (x right, y down).
- **Depth.** The depth the ISP tests is z after the viewport transform. With W-buffering on (a bit
  of the draw's state) it is the viewport's z mapping of 1/w instead, z playing no part. 1/w is
  linear on the screen, so the interpolated depth is exact across a triangle and orders fragments
  by w, as the PICA's W-buffer does under a perspective projection, with no program writing a depth
  ([isp.md](isp.md)). Clipping is unchanged.
- **Blocks.** Primitives are stored in blocks of at most 16 vertices and 24 primitives, with the
  draw's state words beside them.

## Binning (TE)

- Tiles are 32x32 pixels, grouped into macrotiles (a 2x2 or 4x4 grid over the target).
- A primitive goes into a tile when its bounding box, in sixteenths of a pixel, reaches the tile
  and no edge leaves all four of the tile's corners strictly outside it. A primitive whose edge
  lies exactly on a tile boundary therefore also goes into the tile past it, and that tile is
  then stored.
- Each tile gets a list of the primitive blocks it uses, each block masked down to the primitives
  that reach it, and a header in the region array the shading run walks.
- The four cores share the binning. A render's primitives are cut into runs of a set count, and
  the cores take the runs in turn. The split has two modes: Sony's firmware keeps the first, sgxd
  sets the second ([below](#the-clipper-lockup)).

## The clipper lockup

The SGX543's clipping hardware can lock up the binning unit, an erratum of its revision. Sony's
driver recognises it and, on a retail console, restarts the GPU. sgxd has no recovery, so it avoids
the lockup's triggers instead.

What triggers it, from a homebrew that floods the clipper with one kind of triangle:

| Triangles | Result |
|---|---|
| inside the view; past the near plane; one vertex past the guard band; axis-aligned quads at one w; a perspective scene crossing the near plane and the screen edges, as games draw them | ran clean for minutes |
| more than one vertex past the guard band, the vertices at different w | locked up in 10-30 seconds |
| positions no single projection makes (z and w independent, w through zero) | locked up at any guard band width |
| triangles through w = 0 whose vertex behind the eye lies inside the near plane and past the far plane | locked up within frames; the same triangles with that vertex past the near plane ran clean for tens of millions |

The last row is the cause the others share. A vertex behind the eye has w < 0, and where the
mapping from the PICA's depth range puts it decides whether the clipper meets it inside the near
plane.

So sgxd:
- **puts each draw's vertices behind the eye past the near plane.** The PICA's range maps onto
  the TA's as ±(2z + w), and the sign decides which of the PICA's planes becomes the TA's near
  plane: z = 0 for a standard projection, z = −w for a reversed one, the plane that the
  projection's vertices behind the eye lie past. Each draw's sign comes from the rows of its
  vertex program that make z and w. A near plane oblique to both serves neither and keeps the
  standard sign;
- **keeps the guard band as wide as the stored coordinate range allows.** The vertex program
  divides the position's x and y by a factor, and the viewport's scale is multiplied by the same
  factor: every pixel lands where it would, and the band is that many times wider, so far fewer
  triangles are clipped at it;
- **never gives the clipper user clip planes.** A viewport smaller than its target is kept by
  storing only that part of the target, and by the ISP's mask ([isp.md](isp.md));
- **uses the second split mode.** Whether a core locks up depends on what it has clipped since
  its run of primitives started. A frame that locks one core at the same primitive every time in
  the first mode bins clean in the second, with the same pictures as binning on one core.

## The parameter buffer (DPM)

- **Pages.** Binning writes into 4 KiB pages that the DPM takes from a free list: a list of pages
  per macrotile, and one for primitives that span macrotiles. The shading run gives a render's
  pages back at the free list's tail.
- **One buffer for every render,** as Sony's firmware has it. A buffer per render stops the GPU
  as soon as one render's binning overlaps another's shading. Its free pages are loaded whenever
  sgxd takes the GPU.
- **A threshold** caps the pages the binning context may hold, counted over every scene binned and
  not yet shaded. A binning run that reaches it stalls until a shading run gives pages back.
- **The deadlock.** A binning run that runs out of pages while other scenes wait for their shading
  stops the GPU for good: those shading runs wait for it. sgxd keeps it from happening (Sony's
  driver instead raises the limit into a reserve and restarts the binning):
  - the buffer is far larger than any render needs, most of it in main memory. Because pages come
    back at the list's tail, once the list has turned over every page takes its turn, and about
    three quarters of the binning traffic goes to main memory;
  - a binning run starts beside scenes still waiting for their shading only when it is sure of a
    fixed reserve of pages, about four times the largest render measured in any game, and
    otherwise waits for a shading run to give pages back.
