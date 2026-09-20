---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Bound sitting died load 1: Wine XAudio2_8, not xa2_sw

Short sitting on the wrapper-heap-bound DLL (`d3d11.dll` md5
`06702126caa313ac93b30b1cd35d4b88`, linux **11211**). Goal was 5 runs of
1 save + 10 restores, then reload-then-save to look at mutations.
`steam://rungameid/400910` only. CONTINUE beach. GPU=3 TEX_SCALE=1.

First in-world KEY_1 lived (mapid 1, 180 regions, 522.5 MB). `wrapper
heap compacted | largest free 0 KB | retired headers still held: 1
(0.00 MB)`. First KEY_2 started a restore (`room: restoring inside map
1`) and left through `ExitProcess` from `ucrtbase.dll+25B5C` with no
exception — same mixer door as the heaps/mixer sittings. Zero
`resume: done`. Process gone. Five-run protocol did not start.

## It is how XAudio2 was written for Windows

`xa2_sw.c` is a software XAudio2 that assumes the **Windows** search:
Steam does not preload `xaudio2_9.dll`, the game-folder copy wins,
DxLib calls `XAudio2Create` into us. There is deliberately no XAudio2
mixer thread. Output is `waveOut`, which on Windows "brings none of
AUDIOSES or MMDevApi". `xa2_sw_park` can actually stop that thread
before the copy.

This Proton sitting never entered that world.

- Command line is still `-xaudio2`.
- Module roster holds **`XAudio2_8.dll`**, `mmdevapi.dll`,
  `winepulse.drv`, `winealsa.drv`. **`xaudio2_9.dll` is not mapped.**
- Zero `xa2_sw:` lines. `xa2_sw_report` returns immediately when
  `!g_ready`. The software mixer never answered.
- Prefix has `"xaudio2_9"="native,builtin"`. DxLib still bound Wine's
  **XAudio2 2.8** builtin. Game-dir `xaudio2_9.dll` (dated Sep 17, not
  even the bound-build copy) sat unused.

So the live audio peer is Wine's real XAudio2 → mmdevapi → PipeWire,
the one `park_audio_gpu()` was written not to touch:

```
wine: not parking xa2/gpu - waveOutPause and Present wait on wineserver; mixer stays loaded
```

That helper returns on `ss_under_wine()` before `xa2_sw_park()`. Even
if our stand-in were loaded, the Windows park is skipped because Wine
`waveOutPause` waits on wineserver. The mixer thread stays a present
peer. Restore freezes 34 game threads, leaves 15 wineserver-side
running (1 audio still waiting), then mmdevapi calls `ExitProcess`.

`module_excluded` already holds anything named `xaudio*` — correct for
a peer, which is what `XAudio2_8.dll` is. Holding it does not park it.

## Second door: exitpath did not arm

`D3D9SW_FREEZE=0`. `exitpath_arm()` in this tree returns when
`freeze_mode() < 2`, so `RtlExitUserProcess` / `NtTerminateProcess`
were not int3'd. The IAT `ExitProcess` hook logged the leave and the
process actually died (`no exception preceded this, so the process
chose to leave`). Local `bdb4ca7` (845 loads) called `exitpath_arm()`
from `freeze_arm_once()` even at `FREEZE=0`. That lever was not in
this DLL. Stacked with the Wine XAudio2_8 peer, load 1 is the expected
mixer death, not a heap-bound regression.

## What this is not

Not a wrapper-heap measurement. One compact line is not "flat over
tens of pairs". Not cross-session. USER32 not rewound. Exe not dumped.
Game left gone; wrappers left in the snap game dir. Cfg still GPU=3.

Next sitting that wants the five-run protocol needs either the
Windows xa2_sw path actually attached (native `XAudio2_8` name, or
DxLib loading `xaudio2_9`), or the known FREEZE=0 exitpath park, or
both. Forcing native `xaudio2_9` alone does not help if the game binds
`XAudio2_8`.

## Left on disk

`Downloads/frierenserver-gpu-insession/bound-save10.sh`,
`logs/sslog-bound-save10-first-restore-exit.txt`, shots
`bound-inworld.png` / `bound-r1-after-first-save.png`.
