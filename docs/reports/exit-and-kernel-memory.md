# Report: black screen at exit, and kernel memory running out

Observed on a retail PS Vita while testing this fork (built on beta-1) over two days. Neither
problem comes from the fork's changes: `vita_native/` and the GPU takeover code (`sgxd`, see
[sgxd/takeover.md](../sgxd/takeover.md)) are beta-1's, unchanged. They also happen in normal play,
without any test tooling attached.

Setup: firmware 3.65 (Ensō, taiHEN), CapUnlocker loaded at boot, an SD2Vita driver
(`storageturbo`). The app ran Monster Hunter 4 Ultimate (USA) and Monster Hunter XX (Japan).

There appear to be two separate failures.

## 1. Black screen after a clean exit

The app exits normally, as far as its own log goes, and the console then stops responding: the
panel stays black (sound may continue), the PS button does nothing, and only holding the power
button brings it back. Turning the display off and on remotely did not help.

The last lines before the console went unreachable, in two such exits (one from the command
channel, one from the pause menu's Exit):

```
[ 115.864262] Core <Info> core/core.cpp:StopSecondHost:968: second emulation thread stopped
[azaharnative] reback: 128 level-1 entries restored, 0 left to the kernel; 770 page(s) had moved
[ 115.931447] Kernel <Info> core/hle/kernel/process.cpp:~Process:723: Cleaning up process 11
[ 115.998369] Frontend <Info> citra_vita/config.cpp:SetRunningTitle:765: Settings: all games'
[ 116.493577] Frontend <Info> citra_vita/sgxd_present.cpp:Report:182: sgxd: unmap 0x0, 0x0; release 0x0 after 3356 frames
[azahar] azaharnative 102 unloaded: 0x8002d002, its stop 0
```

```
[ 187.132103] Frontend <Info> citra_vita/emu_window_vita.cpp:RunPauseMenu:426: pause menu: closed, action 4
[ 187.133668] Core <Info> core/core.cpp:StopSecondHost:968: second emulation thread stopped
[azaharnative] reback: 128 level-1 entries restored, 0 left to the kernel; 1527 page(s) had moved
[ 187.243361] Frontend <Info> citra_vita/config.cpp:SetRunningTitle:765: Settings: all games'
[ 187.731859] Frontend <Info> citra_vita/sgxd_present.cpp:Report:182: sgxd: unmap 0x0, 0x0; release 0x0 after 4486 frames
[azahar] azaharnative 102 unloaded: 0x8002d002, its stop 0
```

Nothing after these: no kernel message, no error. Most exits end the same way and the shell comes
back; these did not. A guess, not verified: a step of the hand-back (the GPU to Sony's driver, the
shell's 16 MiB of CDRAM, the shell's wake) not completing.

## 2. Kernel memory (ScePhyMemPartKD) running out

At irregular times the kernel cannot allocate from `ScePhyMemPartKD`:

```
Cannot allocate physical page: <try 1M>
	PhyMemPart->name [ScePhyMemPartKD]
	request          0x00100000
Cannot allocate physical page: <Final>
	PhyMemPart->name [ScePhyMemPartKD]
	request          0x00040000
Cannot allocate physical memory from [ScePhyMemPartKD] 0x0003a000
	REQUEST=0x00040000
```

What follows depends on the moment:

- **While a game runs:** the app is closed with the system error **C1-2569-2**.
- **At the next launch:** the console reboots. The log stops between the romfs cache block and the
  plugin's mapping of the guest's video memory:

  ```
  [  17.450320] Common <Info> common/host_shared_memory.cpp:operator():139: memblock azahar-romfs-cache: 16384 KiB PHYCONT_RW -> 0x40010293 at 0x8fa0000
  ```

  In a launch that works, the next line is
  `[azaharnative] guest 1f000000+00600000: the app's attributes, first page par ... -> S 0 TEX 4 C 0 B 0`.
- **After a GPU hang** (one was caused on purpose with a diagnostic switch), the crash's core dump
  failed for the same reason:

  ```
  Cannot allocate physical memory from [ScePhyMemPartKD] 0x00011000
  	REQUEST=0x00010000
  [azaharnative] coredump: create 00000000,
  [sgxd] standby: the owner asked (00000000), the GPU still the owner's after 3000 ms (80028005)
  ```

It does **not** follow the number of launches. Over 25 console boots in the log:

| Azahar launches in that boot | Kernel memory failures | First failure after launch # |
|---|---|---|
| 42 | 6 | 42 |
| 26 | 0 | none |
| 17 | 0 | none |
| 3 | 4 | 2 |
| 3 | 10 | 2 (the console had a Wi-Fi drop and reconnect) |
| 27 | 39 | 1 |

A console restart clears it every time.

`vita_native/src/shellcdram.c` already notes that ScePhyMemPartKD is too small for one of its
blocks, which places it elsewhere; how much of the partition the plugin's other allocations (page
tables, the shell CDRAM lending, coredump buffers) take at peak, and whether any of it outlives an
exit, would be the first thing to measure.

## What would help pin them down

- A reading of ScePhyMemPartKD's free size, printed by the plugin at load, at the take, at the
  hand-back and at unload: a leak, or a peak at one of those steps, would show directly.
- The order of the hand-back steps logged by the plugin up to the shell's wake, to see which one an
  exit that goes black stops at.
