---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver in-session GPU stretch, mixer freeze (2026-09-19)

Follow-on to
[frierenserver-gpu-insession-heaps.md](frierenserver-gpu-insession-heaps.md).
`D3D11SW_GPU=3` `TEX_SCALE=1`. No cross-session. Wrappers only. Simple walk.
`steam://rungameid/400910` only. CONTINUE Rabi Rabi Beach.

The heap-hold lever is closed. This sitting freezes the `mmdevapi` mixer
across restore because Wine GPU/xa2 park is a no-op.

## Named outcomes

**1. SPI size was never the bug. Wine class 5 already fits in 10 KB.**
Local `580e55e` grew the `NtQuerySystemInformation(SystemProcessInformation)`
buffer to 1 MB. linux **104496** (md5 `344d103ef1c3382de5ecdf9050574d84`):
the query succeeds in **10144 bytes** every save and still parses **0/49**
states. Pid 312 is a common DWORD; walking known tids finds them (51 tid
dword hits) but the DWORD at UniqueThread+16 is 0, so `wine_take_state`
rejects it. First tid 316 at SPI +6884:

`0000013c 00000009 00000000 00000000 00000000 00000000 00000000 00000000`

Wine is not filling `KTHREAD_STATE` 1–7 next to `CLIENT_ID`. Unknown-audio
freeze is the path that actually runs.

**2. Unknown-audio freeze is correct and not a wineserver deadlock.**
Every pair logged `audio tid 500 state 0 freeze (spi unknown)` and
`froze 35 game/mixer thread(s), left 14 wineserver-side thread(s) running
(0 audio still waiting, 0/49 states known)`. Park site for tid 500 is
`78A9458B in mmdevapi.dll` (user-mode), not `ntdll`/`win32u`. Suspend of
that thread is a local pause. Held hashes stayed UNCHANGED across every
completed copy.

**3. Freeze alone: 27 loads, then mixer ExitProcess on resume.**
linux **104496**, ~6 s cadence, 173 regions / 521.4–521.6 MB, 49 threads.
Loads 1–27 `resume: done`. Load 27 copy finished, then during
`resume: releasing 49 thread(s)`:

`exit: ExitProcess called from 7B7A5B5C in ucrtbase.dll, 0 frame(s) since the last restore`

stack: `d3d11.dll` hook → `ucrtbase` ×5 → `mmdevapi.dll`. No exception.
Process heap failed validation from pair 2; wrapper heap from pair 3.
Same shape as **91172** / **101830**, death moved from “N frames later”
to “on resume”. Past **101830**’s 7 loads; short of **91172**’s 44.

**4. Ignoring mixer ExitProcess beat the record: 55 loads.**
Local `acda254` (md5 `9cb60b28586b4033c1a0c627a7f2303f`) returns from the
`ExitProcess` IAT hook when `mmdevapi` is on the stack. linux **108167**:
loads 1–55 lived on one process. Two mixer `ExitProcess(3)` calls were
ignored. A third `ExitProcess` from the same `ucrtbase+…5B5C` came with
only two stack frames (hook + `ucrtbase`), so `mixer_on_stack()` missed
it and the real `ExitProcess` ran. Save 56 never started. Zombie
`108167`.

**55 loads is the new TEX_SCALE=1 record** (was 44 on linux **91172**).

## What this is not

Not cross-session. USER32 untouched. `xrestore.ps1` not run. `rabiribi.exe`
not dumped. Cfg still `GPU=3` `TEX_SCALE=1`. Process-heap growth still not
held.

## Next mixer lever

Ignore `ExitProcess` whose immediate caller is `ucrtbase.dll` (Wine system
CRT used by `mmdevapi`; the game’s CRT is MSVCR100). The third death on
**108167** is that caller with a short `CaptureStackBackTrace`. SPI parse
will stay 0/49 until Wine fills ThreadState; do not treat UniqueThread+4
`9` as a known waiter — tid 500 was in user-mode `mmdevapi` when we froze
it.

## Left on disk

`/home/fernserver/Downloads/frierenserver-gpu-insession/`
(`sslog-mixer-freeze.txt` = **104496**; `sslog-mixer-noexit.txt` = **108167**).
Wrappers left in the snap Steam game dir (`acda254`, md5
`9cb60b28586b4033c1a0c627a7f2303f`). Engine commits local
`580e55e` / `acda254` on `cursor/gpu-insession-restore-bd2d` (this box
cannot git push).
