---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Return ACCESS_DENIED from adapter ExitProcess: silent death before resume: done

Iterate-until-die after [bound-save10-helperswallow.md](bound-save10-helperswallow.md) (linux **70105**). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand (`819594c`, md5 `4dabb4720b3a2a106efe83d13e960bfe`). `steam://rungameid/400910` only. CONTINUE beach. Process **72186** gone during restore 1. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper not parked. dinput/win32u not mixer-parked. Process-heap growth not held. HEAPWALK not run. Held Wine heaps not HeapValidated. ctx_verify skipped. Helper `C000001D` still swallowed.

## Door (70105)

Helper swallow: restore 1 `resume: done`, zero helper swallows. winevulkan `+25743` `RtlExitUserProcess` (DXVK `d3d11+251498`, process-heap `0015D348`). Unparking executed ntdll and left (26458). Parking froze Present (16955).

Hypothesis: return `ACCESS_DENIED` (`Eax=C0000022`, stdcall pop, `CONTINUE_EXECUTION`) from adapter `RtlExitUserProcess` / `NtTerminateProcess`. Do not `Sleep(INFINITE)`. Mixer still parks. Steam/dinput/helper still allow-leave.

## Wrapper that ran

`exit_from_adapter` skip no longer calls `exitpath_allow`. It returns to the caller.

## Death: linux **72186**, copy lived, died after resume_all before resume: done

CONTINUE beach. MemAvailable 4.0 Gi → 2.5 Gi in-game. KEY_1 lived: mapid 1, 172 regions, 520.6 MB, 48 threads. Compact 1 header / 0.00 MB. rss 538 MB.

Restore 1: ctx_verify skipped. `load: slot 0, 166 restored, 0 skipped`. Player back x=26084 y=8213. `resume: releasing 48 thread(s)`. `wine: no Flush after resume`. **No** `resume: done`. **No** `exitpath: ... returned to`. **No** `fault:`. **No** helper swallow. Process gone. Fail shot: no window.

`resume_all` returned (the no-Flush line is after it). Next is `dsh_play()` then `xa2_sw_resume()` then `resume: done`. 70105 got through that and logged `resume: done` before winevulkan left. This sitting died earlier and quieter. The ACCESS_DENIED return is the only wrapper delta.

## Protocol

0 of 10 restores of run 1 of 5 (copy started, `resume: done` never logged). Worse than 70105 (1 finished restore). Mutation reload-then-save not reached. Heap bound: compact 1 header / 0.00 MB at first save. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 4.1 Gi after the process left. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan (closed as the freeze: 16955). Do not execute adapter `RtlExitUserProcess` (closed as the leave: 26458 / 70105). Do not return `ACCESS_DENIED` from that door either (closed as the silent death: **72186** died before `resume: done`). Do not Flush then rewind. Do not wait for Present after `request()`. Do not park the helper. Do not mixer-park dinput/win32u. Do not HEAPWALK / HeapValidate held Wine heaps. Do not `GetThreadContext` verify under Wine. Swallow helper `C000001D` (67047 leave closed on 70105).

70105's leave after `resume: done` is still the last *logged* adapter abort (DXVK `d3d11+251498` → `winevulkan+25743` walking `0015D348`). Returning from it made the helper die in `dsh_play` / `xa2_sw_resume` with nothing flushed. Revert the return so the next sitting can see the 70105 door again, or skip `dsh_play` under Wine only if a sitting shows that line as the hang with the return still in.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-adapterret.txt`, `bound-save10-adapterret.out`, shots `adapterret-beach` / `bound-r1-after-first-save`.
Laptop clone `819594c` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` is `4dabb4720b3a2a106efe83d13e960bfe`. Game left gone. Steam snap still up.
