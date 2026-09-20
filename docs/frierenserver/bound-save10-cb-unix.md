---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Close CBs without DXVK unix: Flush and skip-DXVK still park the helper

Iterate-until-die after [bound-save10-handlepage.md](bound-save10-handlepage.md) (linux **43397**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement. Three wrapper changes, three deaths, same sitting. linux **52672** was a false start (title skip-DXVK) and never reached CONTINUE.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. Frozen helpers killed by exact pid. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper wait stayed `YieldProcessor`.

## Door (43397)

Handle pages of every held Wine heap excluded. `partition: consistent`. First restore lived (167 restored, 0 skipped), then the process left during restore 2 with no `RtlExitUserProcess` flushed.

Hypothesis: close DXVK command buffers without putting the helper (or Present) into Wine unix across freeze. Half-open `vkEndCommandBuffer` was the leftover CB story after 37661 named `0C4E7AC0`.

## Death 1: linux **45466**, Flush after resume on the helper

DLL md5 `53aaae39b870365c9c30a456b22bbeb5` (`4d3251e`). CONTINUE beach. KEY_1 lived: mapid 1, 173 regions, 520.6 MB, 48 threads, partition consistent. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`.

First KEY_2 lived: `resume: done`, 167 restored, 0 skipped. Helper Flush after resume returned (`gpu: wine Flush after resume returned`). Then:

```
wine: Flush after resume so load-time CBs close without unix across freeze
resume: done, all threads runnable
fault: C000001D at 60029F4B in d3d11.dll+29F4B, thread 504, 0 frame(s) since the last restore
exitpath: NtTerminateProcess called from ntdll.dll+45622
mixer/ucrt NtTerminateProcess - parking tid 504
```

`d3d11.dll+29F00` is `helper_main`. Tid 504 is the savestate helper. Flush returned, then Wine unix SIGILL'd the waiter. KEY_2 never started load 2. Window frozen. Pid killed.

## Death 2: linux **49235**, Flush on the Present thread after `request()`

DLL md5 `6d386a47328dd01e86b33e528edc8ed4` (`4b569ed`). Helper does not Flush. Present thread Flushes after `request()` returns, so the waiter is not in unix.

KEY_1 lived: same 173 / 520.6 MB / partition consistent. First KEY_2 lived. Present-thread Flush **returned** (`gpu: wine Present-thread Flush after request() returned`). Then the helper parked anyway:

```
wine: helper does not Flush; Present thread will after request() (45466 parked the helper)
resume: done, all threads runnable
wine: Flush on the Present thread after request()
fault: C000001D at 60029F42 in d3d11.dll+29F42, thread 504, 0 frame(s) since the last restore
mixer/ucrt NtTerminateProcess - parking tid 504
```

Any post-resume Flush is unix that SIGILLs the helper, even when the Flush itself is on Present and returns.

## False start: linux **52672**, skip-DXVK on the title auto-save

`SAVE_AT=0` still auto-saves frame 1 on the title. Skip-DXVK hold fired while `gpu_is_up()` was false. Black window, 0 GPU draws. Never CONTINUE. Pid gone before bound-save10. Fixed `1312b2f`: hold only if `gpu_is_up()`.

## Death 3: linux **53818**, skip DXVK between KEY_1 and KEY_2

DLL md5 `33d696a9704f2a9a833bd8542dad6800` (`1312b2f` / `605d074`). No Flush after resume. DXVK begin/draw/end/tex_sync skipped between KEY_1 and KEY_2 once the adapter is up.

KEY_1 lived: mapid 1, 173 regions, 520.6 MB, rss 541368 kB, partition consistent. First KEY_2 lived: 167 restored, 0 skipped.

```
wine: no Flush after resume (49235 SIGILL'd the helper); DXVK was not recording between save and load
resume: done, all threads runnable
fault: C000001D at 60029F6B in d3d11.dll+29F6B, thread 504, 0 frame(s) since the last restore
mixer/ucrt NtTerminateProcess - parking tid 504
```

No `recording resumed`. Load 2 timed out. Ubuntu showed “Steam is not responding” because VEH parked the helper and Present spun in `request()`. CB recording between KEY_1 and KEY_2 is not the leave.

## Protocol

1 save + 1 restore of the 1+10×5, three times (45466, 49235, 53818). Mutation reload-then-save not reached. Wrapper heap `0BFE0000`; compact 1 header / 0.00 MB at first save. Not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable ~3.7 Gi → ~2.6 Gi in-game. No UI lock from memory pressure. 52672 rss ~811 MB on a black title; killed rather than left sitting.

## Next lever

Do not park winevulkan. Do not Flush then rewind. Do not wait for Present after `request()`. Do not rewind unnamed `001D0000`. Do not capture held Wine handle pages. Do not park the savestate helper. Do not Flush after resume (helper or Present). Do not skip DXVK between save and load as the leave.

Catt Man named the remaining seam: **snap is the problem**. Snap Steam confinement turns Wine unix after restore into `C000001D` on the helper wait (`test esi,esi` / `mov eax,g_ctl`). Parking that tid freezes the snap window. Prefix `"steam"` missed `lsteamclient.dll`. `dxgi.dll` trampoline at `0x61000000` was rewound while d3d11 stayed held. Steam launch attributes into the wrappers are that snap path, not a kernel crash reporter.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-liveflush.txt` / `-presentflush.txt` / `-holdrec.txt`, shots `presentflush-*` / `holdrec-*`.
Laptop clone `4d3251e` `4b569ed` `605d074` `1312b2f` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` after 53818 was `33d696a9704f2a9a833bd8542dad6800`. Game left gone.
