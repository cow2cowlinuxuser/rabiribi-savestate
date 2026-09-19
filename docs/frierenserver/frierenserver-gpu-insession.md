---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU restore (2026-09-19)

Goal was Proton in-session save/restore that lives with the adapter path on.
No cross-session this round. Wrappers stayed the artifact. `steam://rungameid/400910`
only. CONTINUE Rabi Rabi Beach. `D3D11SW_GPU=3` `PARTHOLD=1` `EXCLSKIP=1`
`BORDERLESS=0`. linux **82152**.

The previous GPU ladder sitting copied, then died `C0000005` at `ntdll.dll+5025B`
/ winevulkan a few frames after `resume: done` on every `GPU=1/2/3` path.
`GPU=0` + PARTHOLD still lived. Mode 2 (`D3D9SW_REWIND_ALLHEAPS=2`) was rewinding
no-clear-owner heaps `001D0000` and `02070000` because Wine has no `0xFFEEFFEE`
HEAP_SEGMENT, so votes stay at zero. winevulkan stays held and still walked them.

`savestate.c` now leaves unnamed Wine heaps in the present and excludes their
handle pages. Local engine commit `723f4ec` on `cursor/gpu-insession-restore-bd2d`
(this box cannot git push). Live `d3d11.dll` md5 `48523044697e61e0993f7c1dec65ce5f`,
pinned at `60000000`.

## Named outcomes

**1. Unnamed Wine heaps are held, and the partition is consistent.**
Save line: `heap 001D0000 no clear owner left in the present` (same for
`02070000`). Held-hash now fingerprints four heaps (process, wrapper, both
unnamed). `partition: consistent, no captured region sits in a held heap` —
the old `PARTITION LEAK` 1.1 MB on the process heap is gone because Wine held
heaps are excluded by handle pages instead of by missing segments.

**2. GPU=3 in-session restore lives, twice, then a third time, same process.**
`gpu: first frame drawn and presented entirely on the adapter`, then `216
texture(s) released to the adapter, 220.0 MB`. Three KEY_1 / KEY_2 pairs on
pid **82152**, world 1 beach:

| pair | save | load | after |
|---|---|---|---|
| 1 | 161 regions, 409.1 MB, player `x=25914 y=8137` | 155 restored | `resume: done`, no fault |
| 2 | 161 regions, 410.0 MB, player `x=25940 y=8141` | 155 restored | `resume: done`, no fault |
| 3 | 161 regions, 411.5 MB | 156 restored | `resume: done`, no fault |

Held hashes UNCHANGED across each copy, including `001D0000`. Still presenting
after pair 3. Walk-away then restore snaps back onto the beach slope.

**3. The ntdll+5025B / winevulkan death did not return.**
Zero `fault:` lines. The previous sitting's clobber of region 3 at `001D0000`
does not happen when that heap is left in the present.

## What this is not

Not cross-session. USER32/HWND is untouched. `xrestore.ps1` was not run.
`rabiribi.exe` was not dumped. GPU park on Wine is still a no-op
(`wine: not parking xa2/gpu`); holding the unnamed heaps was enough.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/` (shots, sslog, pair
indexes, demo mp4). Wrappers left in the snap Steam game dir. Game left running.
Cfg still `D3D11SW_GPU=3`.
