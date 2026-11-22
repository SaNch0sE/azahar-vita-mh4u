# Threads and cores

The Vita gives an application cores 0 to 2, and azahar unlocks core 3, which it shares with the
system. Each thread role is pinned:

| Core | Threads |
|---|---|
| 0 | the render thread (the rasterizer, sgxd, the presents), Y2R, USB video, shader workers |
| 1 | the second emulation thread (the 3DS's other cores) and the guest it runs, the DSP |
| 2 | the emulation thread (the 3DS's core 0, the emulated kernel and services, the command lists' scan) and the guest it runs |
| 3 | the parse thread, the DSP when core 1 is busy, shader workers, everything else |

- **The guest runs on its emulation thread's core.** A slice turns that core into the guest's
  world ([native-execution.md](native-execution.md)): nothing else of ours runs on core 2, and on
  core 1 only the DSP, beside the second emulation thread.
- **The parse thread** parses the command lists ahead of the render thread, on another core than
  the render thread's, which would otherwise wait for it.
- **The DSP** costs about a fifth of the emulation thread's time. It runs beside the second
  emulation thread, which sleeps most of the time, or on core 3 ahead of the parse, so that a DSP
  frame starts as soon as the game answers its last interrupt.
- **The shader workers** take what the roles above them leave on cores 0 and 3, and core 1 with
  Multicore off. The first tier's worker runs ahead of the second tier's, since a draw may be
  waiting for it ([sgxd/shaders.md](sgxd/shaders.md)).
- **Priorities**, highest first: the emulation threads, the render thread and the audio output;
  the DSP; the parse, Y2R and USB video; the first tier's worker; the second tier's; the rest.
