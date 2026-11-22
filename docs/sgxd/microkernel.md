# Events and the microkernel

The GPU runs its own job queue. A hardware event or a kick from the CPU starts a short program on
core 0's shader core, which finishes the job that was running, starts the next, and ends; the CPU
is interrupted only where it asked to be. Sony's firmware works this way, and sgxd replaces it with
a microkernel of its own.

## Events

- **Each unit latches its completion** in event status registers: the binning run finished, the
  render's last tile stored, the 2D unit done, parameter memory freed, and a software event a
  program can raise. They latch on the master and on the cores, and a host clear clears them.
- **Each event goes to one of two places:** the event data master, which runs an event program on
  core 0's data mover that starts a shader task ([pds.md](pds.md)), or the CPU's interrupt.
- **A kick** from the CPU starts core 0's event program on demand. The register Sony's driver kicks
  with does not start sgxd's event program; the other kick register does.
- **Sony's firmware** also runs on a 1 ms timer. sgxd's runs only on kicks and completions, so an
  event that went missing stops the queue where the stop's readout shows it, rather than costing a
  millisecond unseen.

## Jobs as compiled code

Interpreting a list of register operations on a shader core costs about 10 µs an operation: loading
each value and each address, with the waits. So sgxd compiles each kind of job once into
straight-line USSE code: register writes as immediate stores, waits as short loops, and the values
that change from one send to the next read from a small per-job data block. The code for a job's
shape is made once and kept; sending the job again writes only its data and an entry in the queue.

A kept job holds every register value of the render set-up it was built for, apart from the few that
change from send to send. So a kept job lives exactly as long as its set-up: the set-ups are
recycled when GPU memory runs short, and the next one is usually built at the same addresses, in
main memory and on the GPU, with the same layout. A job kept from the old set-up, found again for
the new one, would run a render with its predecessor's registers: a two-sample render with
one-sample registers never finishes its shading.

The queue and its counters live in memory the CPU sees uncached and the GPU maps cache-consistent,
so neither side flushes anything for the other ([memory.md](memory.md)). The CPU kicks the GPU when
the queue has run dry, and when the binning stream has run out of jobs while the shading stream
waits for its render to end; otherwise the microkernel finds the new job by itself.

## Two streams

Each render is a binning half and a shading half, in two queues:

- The binning stream runs ahead, so a large render's binning overlaps the shading of the renders
  before it. It runs over a shading run in flight as Sony's firmware does, touching none of the
  shading side's registers.
- A shading half waits for its own binning, and binning waits for parameter pages
  ([ta.md](ta.md)).
- The binning half stores its scene's parameter state for the shading half, which loads it.
- Fills, copies and page-table updates go through the queues too, in order with the renders. A
  page-table update is a store into sgxd's own tables followed by an MMU invalidate, made by the
  GPU in queue order ([memory.md](memory.md)).

## Fences and waking the CPU

- **Counters, not objects.** Every job ends by counting itself done; a fence is the number of jobs
  sent, reached when the done count passes it. 2D operations are counted the same way. Each count
  is fenced on both sides: the job's own stores accepted before it, the count accepted before any
  wake.
- **Waking.** The CPU's render thread arms the count it waits for and sleeps. After each count the
  microkernel compares it with the armed one; once that is passed, it clears the arm and raises
  the software event, whose interrupt wakes the thread: one interrupt instead of polling.

## Privileged registers

The shader cores' code bases, the soft reset and the MMU's directory banks take no effect when an
event task writes them itself. Sony's firmware writes them through a supervisor routine: the task
enters a vector programmed for core 0, which makes the write on its behalf and returns. sgxd does
the same, with two details measured on the hardware:
- the vector returns through the next-phase field of its own phase instruction, which has to name
  the instruction after the call;
- the vector does not see the calling task's secondary attributes, so the register and the value
  go through memory.

A soft reset from core 0's shader core reaches cores 0 and 1 only; the reset of every core's data
mover and shader units goes from the CPU, with the GPU idle ([pds.md](pds.md)).

## What an event task can rely on

- **Registers r0 to r35 only.** Whatever the task declares, registers above that hold nothing it
  wrote.
- **The first job after the takeover must store something before its first store fence.** A store
  fence with no store before it, in the first run, never returns.
- **Read the event's inputs before any load and wait.**
- **One task alone is slow** ([usse.md](usse.md)), so a job's compiled code is short.
- **A task polling a register keeps core 0's data mover busy** while it polls.

## When it stops

A wait that runs out stops the microkernel and raises the software event. The CPU then reads where
the microkernel stood (which job, which wait, the last value read) and what each core was doing,
and dumps the GPU's state ([../debugging.md](../debugging.md#gpu-stops)).
