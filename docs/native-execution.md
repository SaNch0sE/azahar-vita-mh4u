# Native execution

The Vita's Cortex-A9 runs the 3DS's ARM11 code directly: there is no recompiler and no
interpreter. A kernel plugin that ships in the vpk (azaharnative) gives the guest the world it
expects.

## The guest's world

What stands in the way is the address space: a 3DS binary expects its code at `0x00100000` and
its heap and stack where the 3DS kernel put them, and on the Vita those addresses belong to the
kernel.

So the guest gets a world of its own: a translation table for the guest's address space and a set
of exception vectors. For each slice of guest execution, an emulation thread calls into the
plugin, which installs both on the calling core, enters the guest in user mode at its own
addresses, and puts Sony's table and vectors back when the slice ends. A slice ends at the first
exception the plugin cannot answer itself (a system call it does not handle, an undefined
instruction, a memory fault) or when a timer bounds it; the emulator handles the reason as an
interpreter would and asks for the next slice.

## The guest's memory

Guest memory is the emulator's own memory, mapped twice: once where the emulator uses it, and once
at the guest's addresses through the plugin's table, page by page over whatever physical pages the
emulator's memory has. Nothing is copied. The emulator tells the plugin whenever the guest maps,
unmaps or protects memory, and code the emulator writes is synced to the instruction cache before
the guest runs it.

The table also holds the emulator's own mappings, so the emulator's code runs inside the guest's
world too (upcalls, below).

## System calls without leaving the guest

Every exit costs a world switch, and games make thousands of system calls a second, so three
layers keep them in the guest's world:

- **The plugin's vector** answers a few calls from what each slice hands it: the system tick, the
  current thread's and process's IDs, and the process data-cache operations.
- **The guest's own code:** a call site the vector answered is patched into a branch to a small
  thunk in the guest's address space, which reads the answer from read-only pages the plugin maps
  for the guest. No exception at all.
- **Upcalls** for the rest: a call site is patched into a branch to a stub that saves the
  registers a C++ function may overwrite, switches to the emulator's stack and thread pointer, and
  calls the emulated kernel's handler at user level, still in the guest's world. A call that
  blocks or wakes a more urgent thread answers with a thread switch, which the plugin makes in
  place. Time spent in the handlers is not counted as guest time. A kernel mutex no other thread
  waits on is lent to the guest, so its lock and unlock run in the stub without an upcall.

## VFP short vectors

The 3DS's FPU runs VFPv2 short vectors (one instruction over several registers); the Cortex-A9
does not, and traps such an instruction as undefined. The first trap patches the instruction into
a branch to a thunk that runs its elements one by one as scalar instructions, so it never traps
again.

## Two emulation threads

A 3DS title runs on two CPU cores (on the New 3DS, up to four). The first emulation thread runs
the 3DS's core 0 and owns everything the cores share: the emulated kernel's objects, the services
and the GPU. The second runs the other cores. Each enters the guest's world on a Vita core of its
own. They share no lock: what one needs the other to do travels as a message in a lock-free queue
of the receiver's. The system calls that touch only the second thread's own cores are answered
there; every other one is forwarded to the first thread.

The 3DS's in-order cores never needed barriers between them, and some games share data across
their cores without any. The A9 reorders memory accesses, so such code can see another core's
writes out of order: a game that then waits forever needs Multicore off.

## Guest memory the GPU uses

Pages the GPU renders into, reads textures from, or the display shows are moved, at the same virtual
address, to memory the CPU does not cache (video memory, or uncached main memory when video memory
runs short), so the CPU's caches need no maintenance around the GPU's work. The GPU's own page table
follows in the GPU's queue order, so nothing waits for the GPU to go idle
([sgxd/memory.md](sgxd/memory.md)).

The guest's access to pages queued GPU work still uses is held (closed to writes, or to
everything) until that work is done. A store that misses every byte the GPU uses is made by the
plugin's exception handler itself, and a page the guest rewrites while the GPU still reads it gets
a second physical page, so the guest goes on at once. A page that queued work is still to move is
the exception: the render side copies it while the holds keep the guest off it, so a store waits
for that work; once the page has moved, stores go past the holds again.

## Rules for the plugin's code

Each of these fails silently at run time:
- **No VFP/NEON the compiler chose.** The kernel does not save the VFP/NEON registers around a
  plugin, and its interrupt handler, kernel threads and exception vector run with another
  context's registers live. Every link checks that only the functions meant to touch VFP state
  hold VFP/NEON instructions.
- **No compiler-made calls in the guest's world.** Code that runs while the guest's table is
  installed reaches nothing of Sony's: a struct copy or a loop the compiler turns into `memcpy`,
  or a 64-bit division, would call into an unmapped kernel there.
- **Never link Sony's `*ForKernel` stubs:** their library IDs change between firmware versions;
  resolve them at runtime through taiHEN.

## Loading

The emulator loads the plugin when it is not loaded yet, then restarts itself once so its imports
bind to it. At exit it unloads the plugin where the firmware allows: a retail console keeps a
kernel module that exports calls to applications loaded until it reboots, so there the plugin
stays, holding nothing, and the next start uses it again. Another version already loaded is
replaced where it can be unloaded; otherwise the emulator asks for a reboot.
