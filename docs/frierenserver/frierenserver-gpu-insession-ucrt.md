---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU stretch, ucrtbase ExitProcess (2026-09-19)

Follow-on to
[frierenserver-gpu-insession-mixer.md](frierenserver-gpu-insession-mixer.md).
`D3D11SW_GPU=3` `TEX_SCALE=1`. No cross-session. Wrappers only. Simple walk.

Record remains **55 loads** on linux **108167** (`acda254`).

## Named outcomes

**1. Returning from `ucrtbase` `ExitProcess` is a dead end.**
Local `4c25cf2` ignored every `ExitProcess` whose caller was `ucrtbase.dll`
(Wine system CRT; game CRT is MSVCR100). linux **114521** (md5
`664057dcd37509daf70229c7b3375fc0`): loads 1–6 `resume: done`, then the
mixer tid called `ExitProcess` from `ucrtbase+25B5C` **2146 times**.
Returning into ucrtbase retries. Then `fault: C0000005 at 7B7A5B5C` on
thread 504, 38 frames after the last restore. Process gone before save 7.

**2. Parking the mixer with `Sleep(INFINITE)` never saw `ExitProcess`.**
Local `aa44a17` marks that tid dead, `Sleep(INFINITE)`s it, and skips it
in `suspend_all` / `resume_all` so a wineserver waiter is not frozen.
linux **116114** (md5 `221f9574fc3e764d8f1e80ca825d5529`): loads 1–9
lived, held hashes UNCHANGED, `resume: done` on load 9, then the process
vanished ~2 s later with **zero** `exit:` lines, zero `parking tid`, zero
`fault:`. UniqueThread+4 `9` stayed rejected (still 0/49 SPI). Beach
in-world confirmed.

The quiet leave matches the existing note in `savestate.c`: the CRT can
reach `RtlExitUserProcess` / `NtTerminateProcess` inside ntdll without
going through the `ExitProcess` IAT. `exitpath_arm()` already int3s those
two, but only when `D3D9SW_FREEZE>=2` (live cfg is `FREEZE=0`).

**3. UniqueThread+4 `9` was not treated as a waiter.**
`wine_take_state` still rejects `st` outside 1..7. Tid 504 froze as
unknown audio, parked in user-mode `mmdevapi` the same as **108167**.

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run. `rabiribi.exe`
not dumped. Cfg still `GPU=3` `TEX_SCALE=1`. Heap-hold unchanged.

## Next mixer lever

Done: see
[frierenserver-gpu-insession-exitpath.md](frierenserver-gpu-insession-exitpath.md).
Arming those two ntdll doors at `FREEZE=0` parked tid 504 and lived **800
loads** on linux **118091**, process still alive.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`sslog-mixer-ucrt.txt` = **114521** retry storm; `sslog-mixer-park.txt` =
**116114** quiet leave). Wrappers left in the snap Steam game dir
(`aa44a17`, md5 `221f9574fc3e764d8f1e80ca825d5529`). Engine commits local
`4c25cf2` / `aa44a17` on `cursor/gpu-insession-restore-bd2d` (this box
cannot git push).
