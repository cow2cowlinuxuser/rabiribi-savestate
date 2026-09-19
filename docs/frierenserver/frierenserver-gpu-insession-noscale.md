---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU restore, TEX_SCALE=1 (2026-09-19)

Retail-size atlases. Same GPU=3 in-session path as
[frierenserver-gpu-insession.md](frierenserver-gpu-insession.md), with
`D3D11SW_TEX_SCALE=1` instead of 2. Wrappers stayed the artifact.
`steam://rungameid/400910` only. CONTINUE Rabi Rabi Beach.
`D3D11SW_GPU=3` `PARTHOLD=1` `EXCLSKIP=1` `BORDERLESS=0` `HEAL=0`.
linux **85980**.

The TEX_SCALE=2 sitting (linux **82152**) lived. Turning the scale off and
retrying the same KEY_1 / KEY_2 pair died immediately after restore:

`fault: C0000005 at 6000C617 in d3d11.dll+C617, thread 320, 1 frame(s) since the last restore`

`esi=3E4CCCCD`, instruction `83 7E 10 01` (`cmp dword [esi+0x10], 1`, the
`kind==texture` test in `gpu_sweep_release`). The read was `3E4CCCDD`
(`esi+0x10`) in a **RESERVED** private page whose allocation base is the
software arena at `20000000`. Full-size CPU planes (~620 MB released vs
~220 MB at TEX_SCALE=2) get decommitted; a garbage `live_next` then looks
like a `Sw11Res` in that reservation.

`d3d11_sw.c` now refuses that walk. `res_live_ok` requires an aligned
header, not in the arena, committed, `kind` 0 or 1, vtable in this
`d3d11.dll`. `res_list_sanitize` cuts a bad link. `gpu_sweep_release`
snapshots up to 512 candidates under `res_list_lock` and uploads outside
it. Live `d3d11.dll` is local `e625f53`, md5 `72cc184386508101b896b9f7a8a6042d`,
pinned at `60000000`. Source is one commit further (`b6ab5f4`, skip
`gpu_shadow_draw` from a dead `src`); that line was not in the DLL this
sitting used.

## Named outcomes

**1. Adapter path at retail atlas size.**
`gpu: first frame drawn and presented entirely on the adapter`, then `216
texture(s) released to the adapter, 620.5 MB`. Arena 768 MB at `20000000`.
Unnamed Wine heaps `001D0000` / `02070000` still left in the present.

**2. GPU=3 TEX_SCALE=1 in-session restore lives, three times, same process.**
Zero `fault:` lines. The sanitizer did not have to cut this sitting (no
`gpu: cutting resource list` lines); the user's `3E4CCCCD` pointer is
unaligned and would have been dropped. Three KEY_1 / KEY_2 pairs on pid
**85980**, world 1 beach:

| pair | save | load | after |
|---|---|---|---|
| 1 | 174 regions, 523.1 MB, player `x=25953 y=8149` | 167 restored | `resume: done`, no fault |
| 2 | 174 regions, 525.7 MB, player `x=25609 y=8137` | 168 restored | `resume: done`, no fault |
| 3 | 174 regions, 527.4 MB, player `x=25609 y=8137` | 168 restored | `resume: done`, no fault |

Held hashes UNCHANGED across each copy, including `001D0000`. Walk-away
then restore snaps back onto the beach ledge. Still presenting after pair 3.

**3. The `d3d11.dll+C617` arena walk did not return on those first pairs.**
The previous session's leftover `ntdll.dll+23BDB` 11224 frames later is a
different death and did not show up on the first three pairs.

**4. A latent collider is in the slot.**
Walking left from the upper beach ledge hits on that same platform with no
visible enemy (HP 34→33 then 30, invuln flash, a pink `4` over empty sand).
Pair 1 already had extra occupied slots at the player's Y: slot 37 at
`x=25888 y=8137` and slot 40 at `x=25671 y=8155` (left of the player). Slot 38
sat directly below at `y=8457`. Restoring the slot again still has the hit.
The entity table is in the snapshot; this is not a present-only GPU miss.

**5. Restore 13 died `ntdll.dll+5025B`, 0 frames later.**
The process took 4 saves and 13 loads. Loads 1–12 resumed. Load 13 copied
(held hashes UNCHANGED, including `001D0000`), then:

`fault: C0000005 at 7BF6025B in ntdll.dll+5025B, thread 316, 0 frame(s) since the last restore`

`reaching: WROTE to 3F800004` — `3F800000` is float `1.0`, page **RESERVED**,
allocation base the software arena at `20000000`. Instruction `89 42 04`
(`mov [edx+4], eax`). Stack is ntdll heap code ← `d3d11.dll+1F7FA` (aligned
`HeapAlloc` of a draw batch) ← software draw. `esi=0C010000` (wrapper heap,
held). This is the same ntdll RVA as the old winevulkan death, but winevulkan
is not on the stack and the unnamed Wine heaps were not painted. Process heap
`00150000` already `FAILED validation` from pair 2 onward; Wine still ran
until this `HeapAlloc` chased a flink that was a float into the arena.

## What this is not

Not cross-session. USER32/HWND is untouched. `xrestore.ps1` was not run.
`rabiribi.exe` was not dumped. GPU park on Wine is still a no-op.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/` (`noscale-*`
shots, `noscale-pair3-demo.mp4`, `logs/sslog-noscale-crash.txt`). Wrappers
left in the snap Steam game dir. linux **85980** is gone. Cfg still
`D3D11SW_GPU=3` `D3D11SW_TEX_SCALE=1`.
