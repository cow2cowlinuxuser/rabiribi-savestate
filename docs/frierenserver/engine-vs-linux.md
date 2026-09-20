---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Statesave engine vs Linux (frierenserver, 2026-09-19)

In-session GPU=3 / TEX_SCALE=1. Proton Experimental. Steam snap. 7.1 Gi
RAM. No cross-session. Wrappers only.

**The copy is good.** Linux ends the sitting around the process (Wine
audio exit, Steam snap scope, host RAM), not inside `resume: done`.

## What the snapshot does right on Proton

- KEY_1 / KEY_2 in-session save/restore completes: `save: slot`, `load:
  slot`, `resume: done`, `fault` 0 for hundreds of pairs.
- Record: **845 loads** on linux **118091**, then **366** more on
  **208448** (rapid pairs then 1-save+5-restore / 5 min). New saves copy
  live state, including CONTINUE overlay. Five restores put that state
  back. Held-hash verdict UNCHANGED every time.
- Unnamed Wine heaps `001D0000` / `02070000` must stay in the present.
  Wine has no `0xFFEEFFEE` HEAP_SEGMENT, so ALLHEAPS=2 was rewinding
  them; winevulkan then walked a rewound heap → `C0000005` at
  `ntdll+5025B` a few frames after resume. Holding them made GPU=3 live
  (TEX_SCALE=2, three pairs on **82152**).
- Wrapper heap `0C010000` is held (`REWIND_SWHEAP=0`). Process-heap
  *growth* must not be held — those spans are the game's ucrt mallocs;
  holding them kills the first restore in `rabiribi.exe`.
- TEX_SCALE=1 needed a wrapper-span hold so ntdll would not write
  `3F800004` into software arena `20000000` during `HeapAlloc`.
- 5-minute idle deaths (CONTINUE) are gameplay. The engine copied them
  faithfully.

## What is Linux / Wine, not the game

**Audio exit path.** After restore, Wine `mmdevapi` calls `ucrtbase`
`ExitProcess`. GPU/xa2 park is a no-op (`waveOutPause` / Present wait on
wineserver). Closed doors: ignore-and-return (2146 retries then
`C0000005`); IAT park without ntdll int3 (quiet leave, 9 loads);
UniqueThread+4 `9` is not a waiter. The lever that lived: int3
`RtlExitUserProcess` / `NtTerminateProcess` at `FREEZE=0`, VEH
`Sleep(INFINITE)`, skip that tid in suspend/resume. Mixer parks around
load 8 (tid 504 / 500) and stays parked.

**wineserver.** Save freezes ~35 game/mixer threads and leaves ~14
wineserver-side running. Load 845 stalled ~3 h between `save: slot` and
`resume: done` with the mixer parked — restore waiting on wineserver is
still open. `xdotool --sync` on a dying window hung the driver for that
stall.

**Steam snap.** The process does not always die with a `fault:`. At 18:35
systemd tore down `snap.steam.steam` after OOM killed `pipewire-pulse`.
At 20:42 the desktop froze under memory pressure and the host was
rebooted. No second `exit:`, no wrapper crash.

**RAM is the limiter on this box.** Wrapper heap is held, so it only
grows: 2 MB → **490 MB** over 845 loads on 118091; **~594 MB** by save
318 on 208448. Plus ~523 MB slot + adapter textures on 7.1 Gi. Copies
still report UNCHANGED. The engine is correct; the laptop is full.

**Input is X11, not savestate.** Dropped KEY_1/KEY_2 are Cursor focus,
ffmpeg shots, stuck KEY_1 edges. `windowraise` + release leftovers, then
click the client. Launch is `steam://rungameid/400910` only.

## Still open

- Confirm the wrapper-heap bound on a **short** sitting: this sitting
  the wrapper heap was `0BFE0000` (not `0C010000`). Compact stayed
  1 header / 0.00 MB at first save. Not measured across tens of pairs.
  See [bound-save10-handlepage.md](bound-save10-handlepage.md).
- Adapter door: do not park winevulkan (closed: 26458 left). Do not
  Flush before rewind (closed: 20929 / 24023 helper, **35034**
  presenter). Do not wait for Present after `request()` (closed: 28336 /
  31417 / 33277). Do not rewind unnamed `001D0000` (closed: 37661
  warning gone). Do not capture held Wine handle pages (closed: 39948
  partition consistent). Do not park the savestate helper (closed:
  43397 left). Do not Flush after resume (closed: 45466 helper, 49235
  Present). Do not skip DXVK between KEY_1 and KEY_2 as the leave
  (closed: 53818 still parked). Close CBs without DXVK unix across freeze
  is closed as the leave; snap Steam confinement is the named seam.
  Unparking the helper (`f04e7e3`) moved the freeze onto dinput tid 492
  at `resume: releasing` (linux **57984**). 37661 leftover rewind
  `0C4E7AC0` / `0C4D71B0`. See
  [bound-save10-cb-unix.md](bound-save10-cb-unix.md),
  [bound-save10-snap.md](bound-save10-snap.md).
- What restore waits on while the mixer is `Sleep(INFINITE)` (load 845
  stall). The stall may have made RSS worse by leaving the leak running;
  it is not the leak.
- Cross-session. USER32 rewind (intentionally not). Dumping
  `rabiribi.exe` (intentionally not).
