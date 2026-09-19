---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU stretch, held-heap spans (2026-09-19)

Follow-on to
[frierenserver-gpu-insession-stretch.md](frierenserver-gpu-insession-stretch.md).
`D3D11SW_GPU=3` `TEX_SCALE=1`. No cross-session. Wrappers only.

**91172** (local `1fa9e71`) remains the record: 44 loads, then
`ExitProcess` from `ucrtbase` with no stack. This sitting added a short
stack on that path and tried to hold grown spans of every held Wine heap
the same way as the wrapper heap.

## Named outcomes

**1. Holding process-heap growth kills the first restore.**
`HeapAlloc`/`RtlAllocateHeap` remembering plus a Wine header back-pointer
scan held 87 MB of process-heap-shaped private memory (hash 64 KB →
89560 KB). Snapshot 520 → 411 MB. linux **98397** / **99654**: copy
`VERDICT the copy painted held memory`, then
`fault: C0000005 at rabiribi.exe+36932E`, 0 frames after load 1. The
game's ucrt mallocs live in those spans. Handle page + LFH of `00150000`
stay in the present; the rest has to travel. Block restore is still off
on Wine.

**2. Hashing `alloc_span` including the reserved tail is a save-time
C0000005.**
linux **97062** (title-screen save, player `x=0 y=0` world 8):
`d3d11.dll+3FA80` read reserved `0F8D0000` (LFH base `0F860000`) while
hashing held spans. Hash now walks committed pages only.

**3. Wrapper-span-only + ExitProcess stack: same death, now named.**
linux **101830**, local `37e870d`, md5 `099e2d819a8a15054f570392b5cb53c6`.
172 regions / 520.4 MB, process-heap hash 64 KB, held hashes UNCHANGED.
Loads 1–7 resumed. Load 8 began restore (SKIPREG printed), then:

`exit: ExitProcess called from 7B7A5B5C in ucrtbase.dll, 115 frame(s) since the last restore`

`stack: d3d11.dll (the hook) → ucrtbase ×5 → mmdevapi.dll`

No exception. Mixer thread still running: GPU/xa2 park on Wine is a
no-op. Faster cadence (~4 s/pair vs ~6 s on **91172**) hit the same
quiet CRT exit at load 8 instead of 45.

**4. Cadence is not a new bug class.**
Process heap still `FAILED validation` from pair 2; wrapper heap from
pair 3. That is the **91172** shape. The new fact is the leaver:
`mmdevapi`, not the game and not winevulkan.

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run. `rabiribi.exe`
not dumped. Cfg still `GPU=3` `TEX_SCALE=1`.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`sslog-stretch.txt` = **91172**; `sslog-heaps.txt` = **101830**;
`sslog-heaps-scan-crash.txt` = **98397**; `sslog-heaps-title-crash.txt`
= **97062**). Wrappers left in the snap Steam game dir.
