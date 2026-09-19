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
that. Later pairs log `dead mixer tid 504 already parked - leave` (1587
hits by load 800) and freeze 34 game threads. Held hashes stayed
UNCHANGED. UniqueThread+4 stayed `9` and `wine_take_state` still rejects
it (0/49 SPI, 10208 byte query). One `exit:` line the whole sitting.

**3. Dropped KEY_1/KEY_2 during `CONTINUE?` / HP 0 are driver misses.**
Save 97 (25 s), load 269 (40 s), then many more after pair 540: each
timed out, process still `Ssl`, retry completed in ~2 s. Twenty-one
`ok on retry` lines through load 800. Not a mixer death and not a
wineserver stall on the parked tid. Driver now retries both keys.

**4. 800 loads on one process, still alive.**
Same linux **118091**, ~6 s cadence, 173 regions / 522.1 MB / 49 threads.
Loads 1–800 all `resume: done`. Zero `fault:`. Load 400 and load 800
were in-world at Rabi Rabi Beach (HP 50 at 400, HP 22 at 800 — simple
KEY_LEFT between pairs, the game happens). Artificial caps: 200, then
400, then 800. Continue script **196930** picked up at 801 toward 1200
on the same pid.

Load 836: save finished, two KEY_2 retries missed (40 s each) while HP
was 0. Process still `Rsl`. Click-into-client + KEY_2 four minutes later
completed load 836 immediately and restored HP 22. `windowactivate`
alone is not enough on this box once Cursor has the pointer. Driver now
clicks the client before every key. Stretch restarted at 837 toward
1200 (script **202304**). Wrappers left in the game dir. Process not
killed.

**800 loads is the new TEX_SCALE=1 record** (was 55 on linux **108167**).
Still stretching past 836 at the time of this note.

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run.
`rabiribi.exe` not dumped. Cfg still `GPU=3` `TEX_SCALE=1`. Process-heap
growth still not held. UniqueThread+4 `9` still not treated as a waiter.

## Next mixer lever

Keep stretching **118091** until it dies. Dropped keys are closed as a
false death; click the game client before KEY_1/KEY_2. If a later death
is a quiet leave with no `exitpath:` / `exit:` line, the next door is
whatever bypasses both the IAT and those two ntdll int3s. `dsh_quiet` /
wineserver has not stalled a completed save through 836 loads with tid
504 in `Sleep(INFINITE)`.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`sslog-mixer-exitpath.txt`, `shots/exitpath-after-load-400.png`,
`shots/exitpath-after-load-800.png` = beach still in-world). Wrappers
left in the snap Steam game dir (`bdb4ca7`, md5
`9fa2c0794435ac13dfd31d77fa9c7ca1`). Engine commit local `bdb4ca7` on
`cursor/gpu-insession-restore-bd2d` (this box cannot git push).
