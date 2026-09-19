---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU stretch, TEX_SCALE=1 (2026-09-19)

Same adapter path as
[frierenserver-gpu-insession-noscale.md](frierenserver-gpu-insession-noscale.md).
Goal is no longer two pairs: keep saving and loading in one process until it
dies. Simple walk away from spawn. Do not play. No cross-session.
`steam://rungameid/400910` only. CONTINUE Rabi Rabi Beach.
`D3D11SW_GPU=3` `D3D11SW_TEX_SCALE=1` `PARTHOLD=1` `EXCLSKIP=1`
`BORDERLESS=0` `HEAL=0` `REWIND_SWHEAP=0` `REWIND_ALLHEAPS=2`.
linux **91172**.

Load 13 on **85980** died `ntdll.dll+5025B` writing `3F800004` (float `1.0`)
into the software arena at `20000000` during wrapper `HeapAlloc`. Wine has no
`0xFFEEFFEE` HEAP_SEGMENT (`nseg` stays 0), so a held heap's handle page was
excluded and the grown spans were captured as unclaimed private memory and
rewound. `savestate.c` now remembers every `sw_malloc` / `HeapCreate` allocation
base (`g_sw_base[64]`) and `sw_exclude_spans()` holds those too. Draw keeps one
triangle buffer (`tri_batch_get`) so the first present after restore is not a
fresh HeapAlloc. Live `d3d11.dll` is local `1fa9e71`, md5
`55c83fae62d9833b71ffdec00c0ed848`, pinned at `60000000` (DYNAMICBASE cleared
inline; `tools/noaslr.py` is still missing on this clone).

## Named outcomes

**1. Wrapper-span hold moved the death.**
45 saves, 44 `resume: done`, zero `fault:` lines. Loads 1–44 lived on the same
process. That is past the old load-13 `HeapAlloc`-into-arena crash. First save
partition logged `wrapper heap: 3 span(s) remembered, 2 newly excluded` and
`4.3 MB left in the present`. Late pairs grew to `5 span(s) remembered, 2 newly
excluded`. Unnamed Wine heaps `001D0000` / `02070000` stayed UNCHANGED across
every completed copy.

**2. Adapter path at retail atlas size still draws.**
Cfg stayed `GPU=3` `TEX_SCALE=1`. Snapshot ~172 regions / 520.5–520.7 MB, 49
threads, 34 rewound. World 1 beach. Player started `x=25871 y=8137` and was
still on that ledge at `x=25612 y=8137` when load 45 began.

**3. Process heap validation fails from pair 2; wrapper heap from pair 4.**
First save: `5 heap(s) before the save, 0 failed`. After load 1, save 2:
process heap `00150000` `FAILED validation` and never recovered. Wrapper heap
`0C010000` joined it on save 4. Both are left in the present. Held-hash of the
process heap stayed **64 KB** for the whole sitting — handle page only. Wrapper
hash stayed 1088 KB. `RtlValidateHeap` failing is not a crash by itself; the
process kept presenting for 43 more loads.

**4. Load 45 left through `ExitProcess`, no exception.**
Save 45 landed (172 regions, 520.7 MB). Restore began (held hashes listed,
modules unchanged, LFH SKIPREG printed). Then:

`exit: ExitProcess called from 7B7A5B5C in C:\windows\system32\ucrtbase.dll, 198 frame(s) since the last restore`

`address space at death: 3770 MB used, 325 MB free, largest block 115 MB`

`no exception preceded this, so the process chose to leave`

No `abort` / `_amsg_exit` line, so the abort IAT hook did not fire. Caller is
inside ucrtbase. Game threads were frozen; 15 wineserver-side threads (including
12 `d3d11.dll` workers) were still running. GPU park on Wine is still a no-op.

**5. The wrapper-span table is not the whole held heap.**
`nseg` is still 0. Process-heap grown spans have no `sw_remember` equivalent, so
ALLHEAPS=2 still treats them as unclaimed and rewinds them while the 64 KB
handle page stays present. That matches HeapValidate failing from pair 2 and a
quiet CRT exit ~198 frames after load 44, during load 45's copy. Unnamed Wine
heaps are the same 64 KB hash shape; they have not been the death on this path
since they were held.

## What this is not

Not cross-session. USER32/HWND is untouched. `xrestore.ps1` was not run.
`rabiribi.exe` was not dumped. The invisible collider from **85980** was not
re-probed; this sitting was stretch-until-death, not play.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/` (`stretch-*` shots,
`logs/sslog-stretch.txt`). Wrappers left in the snap Steam game dir. linux
**91172** is gone. Cfg still `D3D11SW_GPU=3` `D3D11SW_TEX_SCALE=1`.
