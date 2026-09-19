---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU stretch, ntdll exitpath park (2026-09-19)

Follow-on to
[frierenserver-gpu-insession-ucrt.md](frierenserver-gpu-insession-ucrt.md).
`D3D11SW_GPU=3` `TEX_SCALE=1`. No cross-session. Wrappers only. Simple walk.
`steam://rungameid/400910` only. CONTINUE Rabi Rabi Beach.

Record was **55 loads** on linux **108167** (`acda254`). Returning from
`ucrtbase` `ExitProcess` is closed. Mixer `Sleep(INFINITE)` without ntdll
int3 left quietly after 9 loads.

## Named outcomes

**1. Arming `RtlExitUserProcess` / `NtTerminateProcess` at `FREEZE=0` is
the lever that beat 55.**
Local `bdb4ca7` (md5 `9fa2c0794435ac13dfd31d77fa9c7ca1`) calls
`exitpath_arm()` from `freeze_arm_once()` on the first save even when
`D3D9SW_FREEZE=0`. Both doors int3. The VEH parks the caller with
`Sleep(INFINITE)` and marks the tid dead so `suspend_all` / `resume_all`
skip it. Do not return from `ExitProcess`. linux **118091** (windows pid
**316**): both doors armed on save 1.

**2. The mixer `ExitProcess` parked tid 504 around load 8, and the
process kept going.**
Same `ucrtbase+25B5C` ← `mmdevapi` stack as **108167** / **114521**.
Hook logged `mixer/ucrt ExitProcess(3) - parking tid 504` during
`resume: releasing 49 thread(s)` of load 8. Zero `fault:` lines the
whole sitting. One `exit:` line (that park). 1677 later pairs logged
`dead mixer tid 504 already parked - leave`. Held hashes UNCHANGED.
UniqueThread+4 stayed `9` (still rejected). Wrapper heap grew 2 MB →
**490 MB** and stayed UNCHANGED across every copy.

**3. Dropped KEY_1/KEY_2 are driver misses, not mixer death.**
Save 97, load 269, load 836, save 839, and 27 `ok on retry` lines.
`windowactivate` alone is not enough once Cursor has the pointer. A
stuck KEY_1 (no rising edge) also misses; `windowraise` + release
KEY_1/KEY_2/KEY_LEFT then press. Load 840 was in-world beach, HP 22.

**4. 845 loads, then a quiet leave when the Steam snap scope died.**
Loads 1–844 `resume: done` on schedule (~6–8 s). Save 845 finished
(522.1 MB, 173 regions, 49 threads). KEY_2 at ~15:19 did not print
`resume: done` within 40 s (`wait_sslog` mark 297905). sslog next
grew at **18:35:11**: `load: slot` + `resume: done` (load 845), held
hashes UNCHANGED, usual process/wrapper heap check FAILED. Two seconds
later the driver saw no `rabiribi.exe`. systemd:

`snap.steam.steam-…scope: Consumed 7h 28.561s CPU time` at 18:35:18.

No second `exit:`, no `fault:`, no OOM in dmesg. The parked mixer int3
did not fire again. This is Steam/snap tearing the scope down after a
~3 h stall on load 845, not a return from `ExitProcess`. Continue
script **204233** (840→1200) was gone with it. Linux **118091** is
gone. Wrappers left in the game dir.

**845 loads is the TEX_SCALE=1 record** (was 55 on linux **108167**).

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run.
`rabiribi.exe` not dumped. Cfg still `GPU=3` `TEX_SCALE=1`. Process-heap
growth still not held. UniqueThread+4 `9` still not treated as a waiter.

## Next mixer lever

A new same-box sitting on a fresh process (118091 is gone). Load 845
stalled ~3 h between `save: slot` and `resume: done` with tid 504 in
`Sleep(INFINITE)` — wineserver vs parked mixer is back on the table
for that stall, distinct from the Steam scope teardown that followed.
If the stall repeats, the next door is whatever the restore waits on
while the mixer is parked (not another ExitProcess IAT). Do not use
`xdotool --sync` on a possibly dead window (that is how the driver
sat on load 845 until 18:35).

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`logs/sslog-mixer-exitpath.txt` = 298026 lines, linux **118091**;
`shots/exitpath-after-load-840.png` = beach HP 22). Wrappers left in
the snap Steam game dir (`bdb4ca7`, md5 `9fa2c0794435ac13dfd31d77fa9c7ca1`).
Engine commit local `bdb4ca7` on `cursor/gpu-insession-restore-bd2d`
(this box cannot git push).
