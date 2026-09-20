---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Do not park winevulkan; Flush-then-rewind deadlocks; save-only Flush does not stop the abort

Iterate-until-die after [bound-save10-xa2-winevulkan.md](bound-save10-xa2-winevulkan.md) (linux **16955**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement. Three wrapper changes, three deaths, same evening.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. Frozen windows were killed by exact pid so the laptop UI was not left locked. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run.

## Door close (16955)

Do **not** park `RtlExitUserProcess` / `NtTerminateProcess` when winevulkan is on the stack (EBP frame 0 on 16955 was `winevulkan.dll+18E57`). Restore the original ntdll byte and continue into the real exit. Mixer/ucrt still park.

To stop the adapter wanting to leave: `ID3D11DeviceContext::Flush` (vtable 111) without Present, from `gpu_park` on Wine, before `suspend_all`. Hypothesis: freeze while a DXVK command buffer is still recording; after resume `vkEndCommandBuffer` throws "Exception in Unix call" and `ExitProcess(3)`.

## Death 1: linux **20929**, Flush on save **and** load, hung at resume

DLL md5 `2da9e9c1c6bd467b0d82504c18acfeea` (`45a1873`). Probe attached. `exitpath: ... mixer/ucrt will park; winevulkan will not`.

In-world KEY_1 lived: mapid 1, 178 regions, 522.0 MB, 48 threads. Flush returned (`gpu: wine Flush returned`). Compact: 1 header / 0.00 MB.

First KEY_2 copied: `load: slot 0, 172 restored`, player back at x=25783 y=8137, `contexts: 34 set, all read back exactly as set`. Then stuck at `resume: releasing 48 thread(s)`. Zero `resume: done`. Window still showing beach, helper in wineserver. Pid killed.

Flush itself did not hang. Flush-then-rewind-then-`ResumeThread` deadlocks wineserver.

## Death 2: linux **24023**, Flush plus 200 ms drain, hung at SetThreadContext

DLL md5 `ee2f4dd75694c6dfd368afce607a9f79` (`f733bf5`). Save Flush returned and `gpu: wine Flush drain slept`. Save lived.

First KEY_2 Flush returned, freeze ran (`left 13 wineserver-side`), then:

```
WARNING: thread 364 REFUSED the context we set (err 3221225501) - it will resume with present-time registers over rewound memory
WARNING: thread 380 - could not read its context back, so whether the registers took is unknown (err 3221225501)
```

`3221225501` is `C000001D`. sslog stuck at line 484. Never `load: slot`. Never `resume: done`. Drain moved the hang earlier; it did not remove it. Pid killed.

**Flush before a rewind is a dead end.** Save Flush (no rewind) is fine. Load Flush deadlocks wineserver at `SetThreadContext` or `ResumeThread`.

## Death 3: linux **26458**, Flush on save only; presenter allowed to leave

DLL md5 `3b0fbf09d4915cb08c0ac4827431b821` (`9e61e1c`). Load logs `wine: not flushing adapter on load`. Save still Flushes and drains.

In-world KEY_1 lived: mapid 1, 178 regions, 522.0 MB, 48 threads.

First KEY_2 **lived**:

- `contexts` restored
- `witness: player IS back`
- `load: slot 0, 172 restored, 0 skipped`
- `resume: done, all threads runnable`
- heap check 0 failed
- compact 3 headers / 0.00 MB

Immediately after the census that follows resume, same door as 16955:

```
exitpath: RtlExitUserProcess called from 7BE9FBEA (kernel32.dll+FBEA)
  frame 0: winevulkan.dll+18E57
exitpath: adapter RtlExitUserProcess - not parking the presenter
exitpath: NtTerminateProcess called from ntdll.dll+221AE
exitpath: adapter NtTerminateProcess - not parking the presenter
fault: C000001D at ddxx_MesHoooooook.dll+1C51, thread 524, 0 frame(s) since the last restore
```

Process actually left. Present was **not** frozen. KEY_2 never started load 2 because pid 26458 was gone. `bound-save10` reported `process gone during run 1 restore 2`.

MesHook `C000001D` is the abort loop running for real once the ntdll int3 is restored — not a new mixer door. Tid 524 is the same presenter 16955 parked.

Save-only Flush does not keep `vkEndCommandBuffer` closed across a later load freeze. Gameplay after KEY_1 opens new command buffers. Load cannot Flush them without deadlocking wineserver.

## Protocol

1 save + 1 restore of the 1+10×5. Mutation reload-then-save not reached. Wrapper heap not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped. Game left gone; wrappers left in the snap game dir (`xaudio2_8.dll` + override still there). Cfg still GPU=3 TEX_SCALE=1.

## Next lever

The Windows stand-in still restores one Proton frame (`resume: done`, player back) once mmdevapi is out of the process. The remaining door is still the adapter: `winevulkan+18E57` `vkEndCommandBuffer` Unix-call abort after resume.

Do not park that tid (closed: 26458 left instead of freezing Present). Do not Flush before rewind (closed: 20929 / 24023 wineserver deadlock). Close load-time command buffers some other way — or stop freezing while DXVK is recording — without a wineserver Flush on the rewind path.

Related warning, unchanged: `system thread 524 holds 001DCA88` on unnamed heap `001D0000`.

## Left on disk

`Downloads/frierenserver-gpu-insession/bound-save10.sh`,
logs `sslog-bound-save10-flush.txt` / `sslog-bound-save10-drain.txt` / `sslog-bound-save10-nfl.txt`,
shots `flush-r1-load-1-hung.png`, `drain-r1-load-1-hung.png`, `nfl-now.png`, `bound-r1-after-first-save.png`.
Laptop clone commits `45a1873` `f733bf5` `9e61e1c` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
