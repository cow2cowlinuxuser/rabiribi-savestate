---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Swallow helper C000001D: restore 1 lived, winevulkan ExitProcess left

Iterate-until-die after [bound-save10-ctxskip.md](bound-save10-ctxskip.md) (linux **67047**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand (`0d306a4`, md5 `434500d07a19179ea9afa4bab5554b45`). `steam://rungameid/400910` only. CONTINUE beach. Process **70105** left on its own after restore 1. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper not parked. dinput/win32u not mixer-parked. Process-heap growth not held. HEAPWALK not run. Held Wine heaps not HeapValidated. ctx_verify GetThreadContext skipped under Wine.

## Door (67047)

`ctx_verify` skip closed the 64318 hang. Restore 1 `resume: done`. Helper tid 500 `C000001D` at `d3d11.dll+2A742` (`helper_main` `YieldProcessor`). Unparking ran `NtTerminateProcess`; `C0000005` at 0; process gone.

Hypothesis: swallow helper `C000001D` with `EXCEPTION_CONTINUE_EXECUTION`. If the blamed instruction is `pause` (`F3 90`), skip it so a sticky unix cannot livelock the VEH. This is not `Sleep(INFINITE)`.

## Wrapper that ran

`ss_fault_log` returns continue on Wine `EXCEPTION_ILLEGAL_INSTRUCTION` when the tid is `g_helper_tid`.

## Death: linux **70105**, restore 1 lived, adapter ExitProcess

CONTINUE beach. MemAvailable 3.7 Gi → 2.5 Gi in-game. KEY_1 lived: mapid 1, 172 regions, 520.6 MB, 48 threads. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`. rss 538 MB.

Restore 1: ctx_verify skipped. `load: slot 0, 165 restored, 1 skipped`. `resume: done`. Heap check 5 skipped (held Wine), 0 failed. Compact 3 headers / 0.00 MB. Zero `wine: helper C000001D ... swallowed` lines — the 67047 helper unix did not fire.

Then tid **584** (adapter):

```
exitpath: RtlExitUserProcess called from kernel32.dll+FBEA
       frame 0: returns to winevulkan.dll+25743
       frame 1: returns to d3d11.dll+251498   (DXVK, not wrapper 60000000)
                saved ... first word 0015D348  (process heap 00150000, held)
exitpath: RtlExitUserProcess - not parking tid 584 (adapter / winevulkan)
exitpath: NtTerminateProcess - not parking tid 584 (adapter / winevulkan)
fault: C0000005 at EFCDD560 ... thread 500, eax=C000001D
```

Executing the real ntdll (the 26458 unpark) left. Helper AV is teardown. Process gone before restore 2.

## Protocol

1 save + **1** restore of the 1+10×5. Helper unix leave of 67047 did not recur. Mutation reload-then-save not reached. Heap bound: compact 1 header / 0.00 MB at first save, 3 after restore 1. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 4.0 Gi after the process left. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan (closed as the freeze: 16955). Do not execute adapter `RtlExitUserProcess` either (closed as the leave: 26458 / **70105**). Do not Flush then rewind. Do not wait for Present after `request()`. Do not park the savestate helper. Do not mixer-park dinput/win32u. Do not HEAPWALK / HeapValidate held Wine heaps. Do not `GetThreadContext` verify under Wine. Swallow helper `C000001D` (closed as the 67047 leave: 70105 had zero swallows).

Snap unix after a living restore is now DXVK `d3d11+251498` → `winevulkan+25743` → `RtlExitUserProcess`, walking held process-heap `0015D348`. Return `ACCESS_DENIED` from that exitpath so the adapter thread keeps running. Do not `Sleep(INFINITE)` on tid 584.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-helperswallow.txt`, `bound-save10-helperswallow.out`, shots `helperswallow-beach` / `bound-r1-after-first-save`.
Laptop clone `0d306a4` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` was `434500d07a19179ea9afa4bab5554b45`. Game left gone. Steam snap still up.
