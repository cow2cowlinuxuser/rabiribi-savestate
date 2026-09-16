# What is reproducible, and what still moves

Measured on 2026-09-16 against a v1.65 Steam install, with `D3D9SW_GHPIN=1`,
`D3D9SW_GHTRACE`, a reset save file, no player input, a save fired at frame 1200
by `D3D9SW_SAVE_AT` and the window closed at frame 1800 by `D3D9SW_QUIT_AT`.
Reproduce with `tools/detrun.ps1` and `tools/baserun.ps1`.

## The allocation stream is reproducible across launches, reboots and machines

Seven launches in one boot produced allocation traces identical to the byte -
one SHA-256 across all seven files. After a reboot the trace differed in exactly
one line out of 2,645: the header recording the image base. All 2,642 allocation
records - offsets, call sites, ordinals - hashed the same.

A second machine, different hardware, reported the same 2,642 operations against
a heap at the same `50000000`.

This is the gameheap work paying off rather than luck. The game's allocations are
routed into an arena we create and pin, and the trace records offsets within that
arena, so nothing in the stream depends on where Windows put anything. Driver and
CRT churn - which does move, and does differ between machines - lives in other
heaps and cannot perturb it.

Two things had to be held still to get here, and only one of them was ours:

- **The save file.** `reset-save.ps1` before every run. Runs that skip it diverge.
- **The XInput pad.** `xinput1_4.dll` with `D3D9SW_XINPUT=0` answers
  `ERROR_DEVICE_NOT_CONNECTED` for every slot. Note that traces matched at 100%
  both with and without this, in a window that ends before the player exists;
  it is insurance we have not yet collected on. The reason to keep it is that
  the TAS author who measured this game from outside named pad recognition as
  one of two things that change its RNG consumption.

## ASLR here is per boot, not per launch

| | boot 1 | boot 2 |
| --- | --- | --- |
| `baseprobe32.exe` | `00440000` x10 | `00F20000` x10 |
| `rabiribi.exe` | `006A0000` x7 | `00B30000` x3 |

Windows places an image once per boot and reuses that base for every subsequent
load. `tools/baseprobe.c` asks this in a millisecond and needs no window, no
graphics device, no Steam and no DRM, so it can be asked where the game cannot.
It is built PE32 with `DllCharacteristics 0x8140`, the same value the game
carries, so the loader treats it the same way.

The consequence worth separating: **same-boot cross-session restore and
cross-boot restore are different problems.** Within one boot the executable does
not move, so a state saved in one launch and restored in the next sees the same
addresses. Across a reboot it does move, and every pointer into the image is off
by one fixed delta.

## Our own modules are pinned

All four wrappers linked at `ImageBase 10000000`, so three of them were being
relocated on every launch regardless of what the ASLR bit said. They now link at
distinct bases and `tools/noaslr.ps1` clears `DYNAMICBASE` afterwards:

| module | base |
| --- | --- |
| `d3d11.dll` | `60000000` |
| `dxgi.dll` | `61000000` |
| `xaudio2_9.dll` | `62000000` |
| `xinput1_4.dll` | `63000000` |

Verified in the live process across two launches. The allocation trace body
hashed identically before and after, so pinning moved the modules and changed
nothing else.

Only the `DllCharacteristics` bit is edited after linking. `ImageBase` must be
set by the linker, because the loader relocates by
`(load address - header ImageBase)`; rewriting that field afterwards would leave
every absolute address in the code pointing where the linker originally put it.
The relocation table is kept, so an occupied base still loads rather than
failing. This is a preference, not a demand, and it holds only while system-wide
`ForceRelocateImages` is off - `noaslr.ps1` checks and says so.

## The game executable cannot be pinned. Do not retry this

Clearing `DYNAMICBASE` in `rabiribi.exe` - a two-byte header edit, with `.text`,
`.rdata` and the DRM stub untouched - makes Steam refuse to launch it:

```
Application load error 3:0000065432
```

The Steam Stub checksums the file, header included. Restoring the original byte
for byte brings the game straight back. `LARGEADDRESSAWARE` patching works on
many Steam titles and edits the same header region, so this was worth trying; it
does not work here.

Exploit Protection is not an alternative. Its ASLR settings are
`ForceRelocateImages`, `BottomUp` and `HighEntropy` - mandatory ASLR *forces*
randomisation on images that did not opt in. There is no per-process switch to
un-opt an image that set `DYNAMICBASE` itself, so no policy can pin this exe.

## Where that leaves cross-session restore

Same boot: the heap is pinned, the allocation stream is identical, our modules
are fixed, and the executable does not move. Nothing in the address space is
known to move between two launches in one boot. This is testable now with the
existing `D3D9SW_SLOTFILE` path and has not been tested.

Across a reboot: the heap and the allocation stream still hold, and the
executable is off by one delta that we log at both ends and can compute. Pointers
from the heap into the image would need relocating by it. That is a real project,
not a knob, and it should not be started until same-boot restore works.

Still unmeasured: system DLL bases, which move per boot like everything else, and
whether any of this survives contact with gameplay. Every number above was taken
from a window that ends before the player entity exists.
