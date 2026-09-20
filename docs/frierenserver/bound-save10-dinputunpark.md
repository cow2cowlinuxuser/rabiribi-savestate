---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# dinput unpark: two restores lived, load 3 never started

Iterate-until-die after [bound-save10-snap.md](bound-save10-snap.md) (linux **57984**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand (`6be8732`, md5 `8b34507732aadd5e7c672e89fefa4d8f`). `steam://rungameid/400910` only. CONTINUE beach. Frozen process killed by exact pid **61185**. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper not parked. dinput/win32u not mixer-parked.

## Door (57984)

Copy lived, hung at `resume: releasing 48 thread(s)`. `C000001D` at `kernel32.dll+16B40` on tid 492 (dinput / win32u). VEH mixer-parked that tid and froze the snap window.

Hypothesis: do not mixer-park dinput/win32u on that exitpath. Unmask like 43397.

## Wrapper that ran

`exit_from_input` stack-scans `dinput*` / `win32u.dll` the same way `exit_from_adapter` scans winevulkan. Exitpath skip restores the real ntdll byte and `CONTINUE_EXECUTION`. Helper skip and steamish skip stay.

## Death: linux **61185**, restores 1 and 2 lived, load 3 silent

CONTINUE beach. MemAvailable 3.9 Gi → 2.3 Gi in-game. KEY_1 lived: mapid 1, 172 regions, 520.6 MB, 48 threads, partition consistent. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`. rss ~538 MB. dxgi and dinput held.

Restore 1: `resume: releasing` then **`resume: done`**. 166 restored, 0 skipped. Player back x=25958 y=8149. Heap check 0 failed. `gpu: DXVK recording resumed after restore`. Compact 3 headers / 0.00 MB.

Restore 2: **`resume: done` again**. 165 restored, 1 skipped. Then:

```
heap check: heap 00150000 (, in the the process heap heap at 00150000, which we leave in the present) FAILED validation after the restore
heap check: 6 heap(s) after the restore, 1 failed
```

No `fault:`. No `mixer/ucrt`. No `exitpath: ... not parking`. The 57984 `C000001D` during `resume: releasing` did not fire.

Restore 3: KEY_2 and retry produced **no** `load: slot`. sslog stayed at the restore-2 heap-check. Present thread was in `request()` (fail shot is a later beach frame than the save: Erina and the cat have moved, HP still 102). Helper never logged the third copy. Pid still up, State S, rss flat. Killed.

## Protocol

1 save + **2** restores of the 1+10×5. Further than 57984 (0 finished restores) and 53818 (1 then helper park). Mutation reload-then-save not reached. Heap bound: compact 1 header at first save, 3 after restore 1, 0.00 MB. Not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 2.3 Gi in-game, 3.9 Gi after kill. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan. Do not Flush then rewind. Do not wait for Present after `request()`. Do not rewind unnamed `001D0000`. Do not capture held Wine handle pages. Do not park the savestate helper. Do not Flush after resume. Do not skip DXVK as the leave. Do not mixer-park dinput/win32u (closed as the freeze: 61185 `resume: releasing` completed twice).

Process heap `00150000` is held and still **FAILED validation** after restore 2. Load 3 never logs. Do not HEAPWALK that heap (taking a corrupt heap's lock has killed this process). Do not hold process-heap *growth* (that dies load 1 in `rabiribi.exe`). 37661 leftover rewind `0C4E7AC0` / DXVK walking `0015D390` is the same heap.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-dinputunpark.txt`, `bound-save10-dinputunpark.out`, shots `dinputunpark-beach` / `bound-r1-after-first-save` / `bound-r1-load-3-fail`.
Laptop clone `6be8732` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` was `8b34507732aadd5e7c672e89fefa4d8f`. Game left gone. Steam snap still up.
