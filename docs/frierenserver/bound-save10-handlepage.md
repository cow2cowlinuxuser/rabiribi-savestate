---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Handle pages held: partition consistent, first restore still leaves

Iterate-until-die after [bound-save10-unnamed-heaps.md](bound-save10-unnamed-heaps.md) (linux **37661**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement. Two wrapper changes, two deaths, same sitting.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. Frozen helper was killed by exact pid **39948**. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run.

## Door (37661)

Unnamed Wine heaps `001D0000` / `02070000` held. `001DCA88` rewind warning gone. First restore lived, then `winevulkan.dll+23434` `RtlExitUserProcess`. DXVK still walked `0015D390` on process heap `00150000` **restored from the save**.

Wine has no `HEAP_SEGMENT`, so only unnamed handle pages were excluded. PARTITION LEAK 2 regions / 1.1 MB was the process-heap (0.06 MB) and wrapper-heap (1.06 MB) handle pages, captured anyway.

Hypothesis: exclude the handle page of **every** Wine heap with `heap_ours=0`. Not the header-list walk (process-heap growth killed load 1 in `rabiribi.exe`). linux **82152** (TEX_SCALE=2) lived when the partition was consistent.

## Death 1: linux **39948**, handle pages held, helper Sleep parked the waiter

DLL md5 `6f78da3fc4df2352f8e526fa98a5ec65` (`5187d0c`). CONTINUE beach. KEY_1 lived: mapid 1, **173** regions (37661 was 175), 520.6 MB, 48 threads.

```
partition: consistent, no captured region sits in a held heap
```

PARTITION LEAK closed. First KEY_2 **lived**: `resume: done`, player back x=25914 y=8137, 165 restored, 2 skipped. Present kept going (HP 94 → 76, walked left). Then:

```
fault: C000001D at 7BC219F4 in kernelbase.dll+619F4, thread 500, 0 frame(s) since the last restore
  frame 0: returns to 60029F54 in d3d11.dll+29F54
exitpath: NtTerminateProcess called from ntdll.dll+45622
mixer/ucrt NtTerminateProcess - parking tid 500
```

`d3d11.dll+29F00` is `helper_main`. `+29F54` is its `Sleep(1)` wait. Tid 500 is the savestate helper, not xa2. Parking it as mixer meant KEY_2 could never start load 2. Window frozen on the last frame. Pid killed.

Helper `Sleep(1)` is wineserver. After restore it hit `C000001D` in kernelbase, then `NtTerminateProcess`, then the mixer park swallowed the waiter.

## Death 2: linux **43397**, helper YieldProcessor, process actually left

DLL md5 `0bbaf0c20ad42e11273ccc9b40e7fc60` (`59cd446`). Helper wait is `YieldProcessor`, like `request()`. CONTINUE beach.

KEY_1 lived: mapid 1, 173 regions, 520.6 MB, partition consistent. Compact 1 header / 0.00 MB.

First KEY_2 **lived**: `resume: done`, 167 restored, **0 skipped**, player back. `d3d11_sw.log`: `savestate restored slot 0 in 586.8 ms`. Then process gone during restore 2. sslog ended at the post-restore census. **No** `RtlExitUserProcess` line flushed. No winevulkan frames. Quiet leave, same shape as 37661/26458, faster than the log.

YieldProcessor unmasked the leave. 39948 had hidden it by parking the helper.

## Protocol

1 save + 1 restore of the 1+10×5, twice. Mutation reload-then-save not reached. Wrapper heap `0BFE0000` this sitting (not `0C010000`); compact 1 header at first save, 3 after first restore, 0.00 MB. Not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 3.9 Gi → ~2.5 Gi in-game → 4.1 Gi after 43397 left. No UI lock from memory pressure.

## Next lever

Do not park winevulkan (26458 left). Do not Flush then rewind (20929 / 24023 / 35034). Do not wait for Present after `request()` (28336 / 31417 / 33277). Do not rewind unnamed `001D0000` (37661 warning gone). Do not capture held Wine handle pages (39948 partition consistent). Do not park the savestate helper (43397 left).

First restore still lives, then the process leaves. Close CBs without putting DXVK into unix calls across freeze remains open. 37661 named the leftover rewind `0C4E7AC0` / `0C4D71B0` (restored from the save, not the process-heap handle page). 43397 died before that report flushed.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-handlepage.txt` / `-hyield.txt`, shots `handlepage-beach` / `handlepage-after-restore1` / `hyield-beach`.
Laptop clone `5187d0c` `59cd446` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` is `59cd446` (`0bbaf0c20ad42e11273ccc9b40e7fc60`). Game left gone.
