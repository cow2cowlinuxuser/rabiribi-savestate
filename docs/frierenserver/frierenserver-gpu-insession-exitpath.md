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
**316**): both doors armed on save 1:

`exitpath: RtlExitUserProcess at 7BF32180 will park the caller instead of ending the process`
`exitpath: NtTerminateProcess at 7BF1D384 will park the caller instead of ending the process`

**2. The mixer `ExitProcess` parked tid 504 around load 8, and the
process kept going.**
Same `ucrtbase+25B5C` ← `mmdevapi` stack as **108167** / **114521**.
Hook logged `mixer/ucrt ExitProcess(3) - parking tid 504` during
`resume: releasing 49 thread(s)` of load 8. Zero `fault:` lines after
that. Later pairs log `dead mixer tid 504 already parked - leave` and
freeze 34 game threads (15 left running, the extra skip is the parked
mixer). Held hashes stayed UNCHANGED. UniqueThread+4 stayed `9` and
`wine_take_state` still rejects it (0/49 SPI, 10208 byte query).

**3. 96 loads on the first pass, then a dropped KEY_1 — not a process
death.**
`stretch-mixer.sh 120` lived loads 1–96 (~6 s cadence). Save 97 timed
out at 25 s (`wait_sslog timeout pat=save: slot mark=34383`). Process
still `Ssl` with the window up. sslog had stopped at load-96 clobber:
no `save: after dsh_quiet`, no new `exit:`. HP was already 0 at load 90
(beach, NOVICE). After four minutes idle the window showed
`CONTINUE?`. Activate + KEY_1 then produced save 97 / load 97 immediately.
The 25 s miss was the death/CONTINUE overlay, not a wineserver stall on
the parked mixer. 88 pairs had already completed with tid 504 in
`Sleep(INFINITE)`.

**4. CONTINUE YES and an in-world resave, then stretch resumed on the
same process.**
KEY_Z on YES returned to Rabi Rabi Beach with HP 114. KEY_1 rewrote slot
0 in-world (522.1 MB, 173 regions, 49 threads). Continue script picked
up at pair 107 toward 200 on the same linux **118091**. Wrappers left in
the game dir. Process not killed.

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run.
`rabiribi.exe` not dumped. Cfg still `GPU=3` `TEX_SCALE=1`. Process-heap
growth still not held. UniqueThread+4 `9` still not treated as a waiter.

## Next mixer lever

Keep stretching **118091** until it dies or the pair count stops growing.
The parked mixer is not the save-97 miss. If a later death is a quiet
leave with no `exitpath:` / `exit:` line, the next door is whatever
bypasses both the IAT and those two ntdll int3s. If Present hangs with
tid 504 in `Sleep(INFINITE)`, then `dsh_quiet` / wineserver is back on
the table — it has not shown up yet.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`sslog-mixer-exitpath.txt`, `shots/exitpath-after-load-*.png`,
`shots/exitpath-still-alive.png` = CONTINUE?,
`shots/exitpath-after-continue-yes.png` = beach after YES). Wrappers
left in the snap Steam game dir (`bdb4ca7`, md5
`9fa2c0794435ac13dfd31d77fa9c7ca1`). Engine commit local `bdb4ca7` on
`cursor/gpu-insession-restore-bd2d` (this box cannot git push).
