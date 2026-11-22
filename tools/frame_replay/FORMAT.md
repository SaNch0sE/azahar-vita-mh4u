# The frame capture format (version 3)

azahar's diagnostics build writes `host0:vprof-N.vprof` (sgxd_flags `vprof`). Everything is
little-endian. This is version 3; version 2 had no `RSTA` and `PRES` (below).

## File

    u32 'VPRZ'   u32 version (1)   u32 unpacked size
    zstd frame: the unpacked stream

The unpacked stream starts with `u32 'VPRF'`, `u32 version`, then records:

    u32 tag (four ASCII bytes)   u32 size   payload[size]   zero padding to 4 bytes

Times are the Vita kernel's system time in microseconds, low 32 bits. A reader skips the records it does not know.

## Events

| tag | payload |
|---|---|
| `INFO` | `u32 0`, text lines `key=value`: `rom`, `guest_name`, `start`, ... |
| `THRD` | `u32 thread id`, name |
| `TAGN` | `u32 tag id`, event tag name |
| `EVTS` | events, 16 bytes each: `u32 time, u32 thread id, u32 tag id, u32 arg` |

Event tags of note: `op`/`done` (a render thread op, its kind: 0 command list,
1 register write, 2 fill, 3 transfer, 4 DMA), `present`/`presented`, `vblank`, `idle`,
`pass` (an sgxd pass kicked, its number), `gpu_ta`, `gpu_ta_end`, `gpu_3d`, `gpu_3d_end` (the GPU's
stamps, `arg` the stamp's time: the renders the microkernel runs carry none), `sync`/`sync_end`, `sgx_present`/`sgx_present_end`, `finish`,
`irq`, `post`, `push`.

## Frame records

One frame, vblank to vblank, in the order things happened. Emulation thread records come
between `FRAM` and `VBLK`; render thread records follow for the ops the frame queued.

| tag | payload |
|---|---|
| `FRAM` | `u32 frame, u32 time` |
| `PSTA` | the PICA's state at the frame's start (below), written by the frame's first op |
| `GSPC` | a GSP command: its 8 words with the addresses physical, `u32 time` |
| `EREG` | a GPU register write: `u32 index, u32 value, u32 lcd, u32 time` (index into PicaCore::Regs, or RegsLcd when `lcd`) |
| `EDMA` | a GSP DMA: `u32 time, u32 runs`, runs × `u32 src, dst, size` (physical) |
| `OPQ ` | an op queued: `u32 seq, u32 kind, u32 time` |
| `OPB ` / `OPE ` | op `seq` begins / ends on the render thread: `u32 seq, u32 time` |
| `MEML` | guest memory the op read: `u32 seq, u32 kind, u32 paddr, u32 size, u32 blob` (kind: 0 command list, 1 vertices, 2 indices, 3 texture, 4 colour, 5 depth, 6 DMA source) |
| `PROG` | a shader program before the first draw that uses it: `u32 which` (0 VS, 1 GS), code[4096], swizzle[4096] |
| `DRAW` | `u32 seq, pass, draw, vertices, time`; `u32 n` × (`u32 index, u32 value`): registers changed since the last draw; VS then GS uniforms: `u32 n` × (`u32 index, f32 x, y, z, w`) changed, `u32 bools`, 4 × `u32 int` (x \| y << 8 \| z << 16 \| w << 24) |
| `PASS` | an sgxd pass kicked in an op: `u32 seq, pass, target, width \| height << 16, draws, sync, time` |
| `PTIM` | a recorded pass's own times (it runs synchronously while the frame is recorded): `u32 seq, pass, ta_us, d3_us`; `frame-replay frame` lists the passes by 3D time |
| `RSTA` | the renderer's state at the frame's start (below), after `PSTA` in the same op |
| `PRES` | a result: `u32 seq, index, kind, addr, width, height, format, blob` (below) |
| `VBLK` | `u32 frame, u32 time` |
| `BLOB` | `u32 id`, the bytes (a range's contents, stored once). The blobs stop at their region's end (azahar's `BlobRegion`, 80 MiB): later `MEML` and `PRES` records carry blob `0xffffffff` and no bytes, and such a frame cannot be replayed |

`PSTA`: `u32 n`, regs[n] (PicaCore::Regs); `u32 m`, lcd[m]; then VS and GS each: code[4096],
swizzle[4096], 96 × 4 f32 float uniforms, `u32 bools`, 4 × `u32 ints`, `u32 entry point`; 16 × 4
f32 default attributes; proctex noise[128], colour map[128], alpha map[128], colour[256],
colour difference[256] (raw words); lighting LUTs 24 × 256; fog LUT 128.

`RSTA` is what the frame's passes can start from that is not in guest memory. A pass the frame before left open is ended first. Words:
`u32 version (1), clear order, n`, n × (`start, end, value, bytes, order`): the cleared ranges
no pass has drawn over yet; `u32 generations`; `u32 n`, n × (`addr, width, height, format, bpp,
passes, synced, guest_newer, generation, blob`): the surfaces a pass drew, the blob their
A8B8G8R8 pixels; `u32 n`, n × (`addr, width, height, format, valid, bytes, blob`): the depth
stores, the blob the store's samples (`width × height × 5` with stencil, else `× 4`, both padded
to 32).

`PRES` is what the console made of the frame, for `frame-replay check`. While a frame is recorded
every pass runs synchronously, and each ended pass writes its surface
(kind 0: `format` 0, `width × height × 4` A8B8G8R8, row 0 the PICA's row 0). Each display
transfer writes its output as it landed in guest memory after the GPU was idle (kind 1: linear,
`format` the PICA colour format: 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4). `index` counts
the frame's results from 0. The synchronous passes change the frame's timing: the capture's GPU
times are for passes run one at a time.

## Replay

A frame replays through azahar's own GPU path on the console, in the replay player: azahar's `citra_vita_player` (title AZRPLAY01,
diagnostics builds), with the capture and its requests in the vpk as `app0:replay.vprof` and
`app0:replay.txt`. It runs no title: only memory, timing and the GPU exist, as Razor replays a capture without the game. The emulator's own vpk
does the same when its launch parameter ends in `.vprof`.

A frame replays by restoring `PSTA` (queued as the vblank's op, as in the capture), then walking
the emulation thread records: `GSPC` is executed as GSP would (its addresses are physical
already), `EREG` written as the guest wrote it, `EDMA` run as the DMA. Each of those queues the
same render thread ops as in the capture, in the same order, so op `seq` of the replay is op
`seq` of the capture: before it runs, its `MEML` ranges of every kind are written to guest
memory where they differ, and the textures in them are untiled again. A range the op has already loaded with the same bytes,
and no other load over it since, is not compared again (a frame can re-read one texture draw
after draw). Guest VRAM, which the CPU reads uncached, is
compared through a DMAC copy into a cached buffer. Each run starts from the capture's `RSTA`: the renderer's surfaces, depth stores and clears are
replaced by the recorded ones, so the frame's first pass loads what the console's did. Every
texture is untiled again too, except in `profile`. A capture
without `RSTA` (version 2) starts from whatever the replaying azahar had drawn itself. What the
replay overwrote is put back at `quit`, and the interrupts the replay's ops posted are dropped,
so the guest sees none of them. The passes render synchronously,
except in `profile`.

Requests, one per line: the player's first batch is `app0:replay.txt` (when host0 has no
`vprof-replay.txt`), every later batch `host0:vprof-replay.txt`'s (the file is removed once
read):

| Request | Result records |
|---|---|
| `run N` | the whole frame N times: `RPAS` per pass, `ROPT` per op |
| `costs` | per draw D: the frame up to D's op with the draws after D left out, three times; the best `RCST` |
| `image D...` | `RIMG`: D's colour target right after it |
| `images` | `RIMG` for every draw |
| `results` | the whole frame once: `RPRS` for each pass's surface and transfer's output, as `PRES` |
| `profile N [present]` | the frame N times as the game runs it (passes pipelined, each run ended by the GPU going idle, textures kept from run to run as the game keeps them; `present` shows the screens after it): `RFRM` per run |
| `quit` | guest memory restored, the guest goes on (the player exits) |

Results: `host0:vprof-replay-out-N.bin` = `"VPRY"`, `u32 version`, `u32 unpacked size`, the
zstd-compressed stream `"VPRR"`, `u32 version`, records as above:

| Tag | Payload |
|---|---|
| `RINF` | `u32 draws` |
| `RTXT` | text (each request's time, memory loads that differed) |
| `RPAS` | `u32 run, pass, ta_us, 3d_us` |
| `ROPT` | `u32 run, seq, us` |
| `RCST` | `u32 draw, ta_us, 3d_us, pass` (the last pass: the draw's) |
| `RIMG` | `u32 draw, width, height, colour address`, width × height RGBA8 (the sgxd surface) |
| `RPRS` | `u32 index, kind, addr, width, height, format, bytes`, the bytes (as `PRES`'s blob) |
| `RFRM` | `u32 run, submit_us, idle_us, loads_us, present_us`: the time to hand the frame over (the render thread's work), from the start to the GPU's idle, the memory loads' part (writing the capture's `MEML` ranges and untiling), the present's |

Version 2 added `RPRS`, version 3 `RFRM`. `frame-replay check CAPTURE [--images DIR]` pairs the
k-th `PRES` of a kind and address with the k-th `RPRS` of the same, and counts the pixels that
differ.

`host0:vprof-replay-done-N` marks each result complete. Copied next to
the capture as `CAPTURE.player.bin`, they load with it in `frame-replay`.
