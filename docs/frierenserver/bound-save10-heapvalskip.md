---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Skip HeapValidate of held Wine heaps: first restore hangs in ctx_verify

Iterate-until-die after [bound-save10-dinputunpark.md](bound-save10-dinputunpark.md) (linux **61185**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand (`70ab98a`, md5 `40cb3c408f2bae54526fec2f3872d8b3`). `steam://rungameid/400910` only. CONTINUE beach. Frozen process killed by exact pid **64318**. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper not parked. dinput/win32u not mixer-parked. Process-heap growth not held. HEAPWALK not run.

## Door (61185)

Restores 1 and 2 `resume: done`. Process heap `00150000` FAILED `HeapValidate` after restore 2. Load 3 never logged `load: slot`. `do_load` starts with `heap_check("as the restore begins")`, which calls `HeapValidate` and takes that heap's lock while Present is in `request()`. Wine heap is not Windows HEAP.

Hypothesis: skip `HeapValidate` for heaps left in the present (`GetProcessHeap()` and `heap_ours==0`). Do not HEAPWALK. Do not hold process-heap growth.

## Wrapper that ran

`wine_heap_held()` returns 1 for `GetProcessHeap()` and any classified held Wine heap. `heap_check` skips those. Summary line now includes `skipped (held Wine)`.

## Death: linux **64318**, restore 1 hung in ctx_verify

CONTINUE beach. MemAvailable 3.7 Gi → 2.8 Gi in-game. KEY_1 lived: mapid 1, 172 regions, 520.6 MB, 48 threads, partition consistent. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`. rss ~477 MB.

Skip took: title save `4 skipped (held Wine)`; KEY_1 save `5 skipped`; restore start `5 skipped`, 0.0 ms. No `FAILED validation`.

Copy started. Then:

```
WARNING: thread 376 - could not read its context back, so whether the registers took is unknown (err 3221225501)
```

`3221225501` is `0xC000001D`. Thread 376 was parked at `ntdll.dll+E020` at save. `ctx_verify`'s `GetThreadContext` after `SetThreadContext` returned that Wine unix error. No `load: slot`. No `resume: done`. No `fault:`. No mixer park. Helper never finished the remaining context write-backs. Fail shot is the same frame as the save (HP 94). Pid State S. Killed.

## Protocol

0 of 10 restores of run 1 of 5. 61185's load-3 `HeapValidate` hang did not recur (the skip is why). First restore died earlier, at snap unix on `GetThreadContext`. Mutation reload-then-save not reached. Heap bound: compact 1 header / 0.00 MB at first save. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 2.8 Gi in-game, 4.1 Gi after kill. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan. Do not Flush then rewind. Do not wait for Present after `request()`. Do not rewind unnamed `001D0000`. Do not capture held Wine handle pages. Do not park the savestate helper. Do not Flush after resume. Do not mixer-park dinput/win32u. Do not HEAPWALK / hold process-heap growth. Do not `HeapValidate` held Wine heaps (closed as the load-3 hang: 64318 skipped 5, 0 failed).

Snap unix `C000001D` during restore is now `GetThreadContext` of a frozen game thread in ntdll wait (`ctx_verify` read-back). Same confinement as 57984's dinput park, earlier in `do_load`.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-heapvalskip.txt`, `bound-save10-heapvalskip.out`, shots `heapvalskip-beach` / `bound-r1-after-first-save` / `bound-r1-load-1-fail`.
Laptop clone `70ab98a` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` was `40cb3c408f2bae54526fec2f3872d8b3`. Game left gone. Steam snap still up.
