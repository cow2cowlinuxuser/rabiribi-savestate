---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Native XAudio2_8 attached; first restore lived; winevulkan then parked the presenter

Iterate-until-die after [bound-save10-xaudio2-8.md](bound-save10-xaudio2-8.md) (linux **11211**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement. Two wrapper changes, two deaths, same evening.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. Frozen windows were killed by exact pid so the laptop UI was not left locked.

## Door close (11211)

`d3d11.dll` `80cf082` + native `xaudio2_8.dll` at `64000000`, prefix `"xaudio2_8"="native,builtin"`.

- `exitpath_arm()` runs at `FREEZE=0`. VEH parks with `Sleep(INFINITE)`. IAT `ExitProcess` parks ucrtbase/mmdevapi. `suspend_all` skips those tids.
- DxLib `-xaudio2` bound the game-dir trampoline: `xaudio2_probe.txt` says `attached as ...\XAudio2_8.dll` then `XAudio2Create flags=0 processor=4294967295 - handing it the software engine`. `xa2_sw:` lines exist.

## Death 1: linux **14079**, still Wine waveOut → mmdevapi

DLL md5 `1b2f66fc340c3afdc4c0b15482ae6e8e`. In-world KEY_1 lived (mapid 1, 186 regions, 527.9 MB, 51 threads). Compact: `retired headers still held: 1 (0.00 MB)`.

First KEY_2: `wine: parking software xa2 mixer, skipping waveOutPause`. Restore started inside map 1. Then:

- `wine: froze 35 ... left 16 wineserver-side (3 audio still waiting)`
- system threads: `dinput x1, winmm x1, mmdevapi x2, d3d11 x13`
- mmdevapi tid **516** held pointers into the **game runtime heap**
- `ExitProcess` from `ucrtbase+25B5C` — parked tid 516
- `fault: C000001D` at `d3d11.dll+36E08` (`mov 0x24(%eax,%ecx,4),%edi`) tid 504
- `NtTerminateProcess` parked tid 504
- zero `resume: done`
- window frozen (identical xwd). Pid killed.

Windows `waveOut` does not load MMDevApi. Wine `waveOutOpen` does. Parking `xa2_sw` does not park that peer.

## Change: mix without a device under Wine

`640eef7`: `out_start` skips `waveOutOpen` when `savestate_under_wine()`. Mixer still retires buffers so DxLib's queue drains. No winmm device, so no mmdevapi from our path.

## Death 2: linux **16955**, first restore lived, then winevulkan ExitProcess

DLL md5 `0cb18c590f41d52b9db37df20bcce38d`. Probe attached `XAudio2_8.dll` again. Log: `xa2_sw: Wine waveOut pulls mmdevapi - mixing without a device`.

No `mmdevapi` in the roster. Freeze line: `0 audio still waiting`. System threads: `dinput x1, d3d11 x13`.

In-world KEY_1: mapid 1, 178 regions, 521.9 MB, **48** threads (3 fewer than 14079). Compact still 1 header / 0.00 MB.

First KEY_2 **lived**:

- `contexts: 34 set, all read back exactly as set`
- `witness: player IS back at x=25698 y=8137`
- `load: slot 0, 172 restored, 0 skipped`
- `resume: done, all threads runnable`
- heap check 0 failed
- RSS 546 MB before and after

Immediately after the census that follows resume:

```
exitpath: RtlExitUserProcess called from 7BE9FBEA (kernel32.dll+FBEA)
  frame 0: winevulkan.dll+18E57
  frame 1: d3d11.dll+CE456
mixer/ucrt RtlExitUserProcess - parking tid 524
```

Zero `fault:`. Zero IAT `exit:`. Second KEY_2 never produced a `load: slot` (sslog stuck at line 606). Window frozen. Pid killed.

Parking the ntdll door no longer saved a mixer; it parked a **winevulkan** caller. That tid does not come back, so Present stops.

## Protocol

1 save + 1 restore of the 1+10×5. Mutation reload-then-save not reached. Wrapper heap not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped. Game left gone; wrappers left in the snap game dir (`xaudio2_8.dll` + override still there). Cfg still GPU=3 TEX_SCALE=1.

## Next lever

The Windows stand-in under the name DxLib binds **works** for one Proton restore once waveOut/mmdevapi is not in the process. The remaining door is the adapter: winevulkan calling `RtlExitUserProcess` after `resume: done`. Do not park that tid (it freezes the presenter). Stop it wanting to leave, or treat winevulkan/d3d11 GPU exits as a different door from mmdevapi.

## Left on disk

`Downloads/frierenserver-gpu-insession/bound-save10.sh`,
logs `sslog-bound-save10-xa28.txt` / `sslog-bound-save10-nowave.txt`,
shots `xa28-after-continue.png`, `nowave-inworld-beach.png`, `bound-r1-after-first-save.png`, `bound-r1-load-2-fail.png`.
Laptop clone commits `80cf082` `2555adf` `640eef7` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
