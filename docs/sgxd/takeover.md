# Taking the GPU from Sony's driver

sgxd takes the GPU through the path Sony's driver lets go of it on every suspend.

## The take

1. **Check the driver.** The driver's code and data are fingerprinted, and an unknown build refuses
   the take; the emulator then shows an error. Firmware 3.60's, 3.65's, 3.68's and 3.73's drivers
   are known (the same code under four module identities).
2. **Close the gate.** The driver's suspend gate makes every new call from any process wait. sgxd
   then waits until no call is still inside the driver.
3. **Power state off.** The driver saves its microkernel's state, disables its interrupt and
   releases the GPU's registers, exactly as before a suspend.
4. **Power on for ourselves.** sgxd powers the GPU up and writes the start sequence Sony's driver
   writes after a power-up. It then resets every core's data mover, shader units and caches, sets
   up its own page tables
   ([memory.md](memory.md)), and checks that the GPU answers with the expected core IDs.

The reset matters: the GPU comes to us with the system's programs and data still cached,
and the data mover would go on running an old program at an address it has run
([pds.md](pds.md)).

Nothing of the driver's is freed or changed: its state stays in memory for the hand-back.
Giving the GPU back, at exit or when the emulator crashes, is the system's own resume event: the
driver restarts its microkernel and opens the gate, and the calls that waited go on.

The GPU's clock is set before the take and never changes while the GPU is ours: Sony's clock
callbacks do nothing while the driver is "off".

What the app draws before the take (its start-up notice, which says the system's overlays stop
drawing, and the errors that end a start) goes through libgxm, which the app terminates before the
take: when the take begins, the system's processes are the driver's only users.

The take never comes while the system's UI is over the app: the shell stops at the driver's gate
once the GPU is taken, and a quick menu open at the take would stay on the screen, frozen, with the
app running unseen beneath it. The quick menu's presses also reach the app, so the start-up notice
ignores input while the system's UI is up.

## Suspend

Every application hears of a standby through its power callback, but the system waits only until
the callback has started, and stops the applications about two milliseconds later. So sgxd puts a
handler of its own ahead of every other at the first suspend event: it asks the emulator for the
GPU and holds the standby until the emulator has given it back, three seconds at most. Without it
the hand-back would run only after the resume, on a GPU that has lost its parameter buffer's
state.

A retail console turns the
display off before a standby, and the power callback carries only the display's state; it also turns
the display off after a while without input, with no standby after it. The emulator takes either as
a suspend:

- it pauses the title, stops handing work to the GPU and waits until what it handed over is done.
  A list of GPU work being built is sent and finished first, never held across the suspend: it
  assumes GPU state that the system changes meanwhile;
- sgxd gives the shell its video memory back and the driver its GPU, as at the end of a session,
  and holds every later GPU call of the emulator instead of failing it. The GPU is the system's
  from then on, so the lock screen can draw whether or not a standby follows.

The shell keeps running while its video memory changes hands, so the memory moves a page at a
time: each page copied, the shell's view of it turned to the new place, and whatever the shell
wrote to the old place in between carried over. A copy of the whole range at once would lose the
shell's writes made during it, and stopping the shell would cost the standby that a press of the
power button starts.

On resume the driver owns the GPU for whatever the system shows first: the lock screen, on a
retail console. The emulator takes the GPU again once the application manager no longer reports
the system's interface over the app and the shell's own display plane, which scans the shell's
video memory, is blank: taken with the plane still on, the emulator's contents of that memory
would show over the picture. The GPU is set up as at the start with the emulator's bookkeeping
kept: the parameter buffer's page list written afresh, the microkernel started where it stood with
its counts. The held calls go on, and the title waits paused where it stopped.

The standby also closes every file the emulator holds open; each is opened again on its next use,
at the place it was left. The automatic standby stays off while the emulator runs.

## Living beside the system

While the emulator owns the GPU, other processes' GPU calls sleep at the driver's gate, so no
system overlay can be drawn. The system shell's main thread draws, so the first overlay it starts
puts the whole shell to sleep until the emulator exits. Asleep, the shell stops answering the
application manager, whose watchdog kills a shell that has been silent for 30 seconds on a retail
console, and the console then reboots. So for as long as sgxd owns the GPU:

- the kernel plugin does for the watchdog what the shell's call does, every few seconds, so a shell
  asleep at the gate is never taken for a hung one;
- every shell overlay the system lets an application lock is locked: the PS button, the quick
  menu, the power menu, and the USB, memory card and music player notices;
- the volume keys are taken out of the shell's pad data; the plugin reads them from the system
  controller and sets the system volume itself, as the shell would (the system's own setter takes
  calls only from system programs). HDMI and USB audio have no system volume, and there the keys
  do nothing;
- what cannot be locked, such as the system's notifications, waits for the exit.

## Video memory

An application gets 112 MiB of the Vita's 128 MiB of video memory; the shell keeps the other 16 MiB.
While sgxd owns the GPU, the shell's 16 MiB is lent to the emulator: its contents are saved to main
memory, the shell's own view of that range is pointed at the copy (the shell keeps writing there
meanwhile), and everything is put back at exit before the GPU is given back.
