---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Skip ctx_verify GetThreadContext: restore 1 lived, helper C000001D left

Iterate-until-die after [bound-save10-heapvalskip.md](bound-save10-heapvalskip.md) (linux **64318**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand (`fa881ae`, md5 `539bba876c3394a2b8281915fbf8498a`). `steam://rungameid/400910` only. CONTINUE beach. Process **67047** left on its own after restore 1. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper not parked. dinput/win32u not mixer-parked. Process-heap growth not held. HEAPWALK not run. Held Wine heaps not HeapValidated.

## Door (64318)

Restore start skipped 5 held Wine heaps, 0 failed. Copy started. `ctx_verify` `GetThreadContext` on tid 376 (parked at `ntdll.dll+E020`) returned `err 3221225501` (`C000001D`). No `load: slot`. No `resume: done`. Snap unix, same confinement as 57984, earlier in `do_load`.

Hypothesis: skip `GetThreadContext` read-back under Wine. `SetThreadContext` still stands. Do not ask wineserver to read a frozen trap frame.

## Wrapper that ran

`ctx_verify` returns immediately when `ss_under_wine()`. Restore log: `contexts: 34 set, Wine skipped ctx_verify read-back (64318 GetThreadContext C000001D on a frozen ntdll wait)`.

## Death: linux **67047**, restore 1 lived, helper unix then process gone

CONTINUE beach. MemAvailable ~3.7 Gi → ~2.4 Gi in-game. KEY_1 lived: mapid 1, 172 regions, 520.6 MB, 48 threads, partition consistent. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`. rss 538 MB.

Restore 1: `contexts: 34 set, Wine skipped ctx_verify`. `load: slot 0, 166 restored, 0 skipped`. Player back x=26087 y=8213. `resume: done`. Heap check 5 skipped (held Wine), 0 failed. rss 571 MB. 64318's hang did not recur.

Then helper tid **500**:

```
fault: C000001D at 6002A742 in d3d11.dll+2A742, thread 500, 0 frame(s) since the last restore
exitpath: NtTerminateProcess called from 7BF55622 (ntdll.dll+45622)
exitpath: NtTerminateProcess - not parking tid 500 (savestate helper - parking it freezes the snap window ...)
fault: C0000005 at 00000000 in unknown+0, thread 500
       VERDICT: call through a bad function pointer, return 6002A6F0
       eax=C000001D
```

`d3d11.dll+2A742` is `helper_main`'s `YieldProcessor` wait (`+2A6F0` is the wait loop). Unparking the helper unmasked the leave: Wine unix on the pause, CONTINUE_SEARCH, real `NtTerminateProcess`, then execute 0. Process gone. Restore 2 `wait_sslog` timeout, no window for the fail shot.

## Protocol

1 save + **1** restore of the 1+10×5. Further than 64318 (0 finished restores). Mutation reload-then-save not reached. Heap bound: compact 1 header / 0.00 MB at first save. Not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable ~3.7 Gi after the process left. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan. Do not Flush then rewind. Do not wait for Present after `request()`. Do not rewind unnamed `001D0000`. Do not capture held Wine handle pages. Do not park the savestate helper (closed as the freeze: 43397 / 57984). Do not Flush after resume. Do not mixer-park dinput/win32u. Do not HEAPWALK / HeapValidate held Wine heaps (closed as the load-3 hang: 64318). Do not `GetThreadContext` verify under Wine (closed as the hang: 67047 restore 1 lived).

Snap unix `C000001D` is now `YieldProcessor` of the **unparked** helper after a living restore. Swallow it (`EXCEPTION_CONTINUE_EXECUTION`) so the wait loop retries. Do not `Sleep(INFINITE)`. Do not let `NtTerminateProcess` run on that tid.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-ctxskip.txt`, `bound-save10-ctxskip.out`, shots `ctxskip-beach` / `bound-r1-after-first-save`.
Laptop clone `fa881ae` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` was `539bba876c3394a2b8281915fbf8498a`. Game left gone. Steam snap still up.
