---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Snap Steam: unparking the helper moves the freeze onto dinput

Iterate-until-die after [bound-save10-cb-unix.md](bound-save10-cb-unix.md) (linux **53818**). Catt Man named the seam: **snap is the problem**. Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL copied by hand into the snap game dir (`f04e7e3`, md5 `700965f3b88cad2fb6e997f13e8661f4`). `steam://rungameid/400910` only. CONTINUE beach. Frozen process killed by exact pid **57984**. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run. winevulkan not parked. No Flush then rewind. No wait for Present after `request()`. Helper wait stayed `YieldProcessor`. Helper not VEH-parked.

## Door (53818)

Skip-DXVK between KEY_1 and KEY_2 did not prevent the leave. First restore lived, then `C000001D` on `helper_main` (`d3d11.dll+29F6B`, `test esi,esi`). VEH parked tid 504 as mixer. Ubuntu “Steam is not responding” is that frozen snap client.

Prefix `"steam"` missed `lsteamclient.dll`. `dxgi.dll` trampoline at `0x61000000` was rewound while d3d11 stayed held. Hypothesis: do not park snap Steam / lsteamclient / the savestate helper on exitpath; do not freeze lsteamclient threads; hold dxgi with d3d11.

## Wrapper that ran

`ss_steamish_name` matches substring `"steam"` and `gameoverlay`. `wine_freeze_this` and `module_excluded` use it. Exitpath skip (restore real ntdll byte, `CONTINUE_EXECUTION`) if adapter / snap Steam / `g_helper_tid`. dxgi held when `!sw_heap_rewound()`. `find_game_dir` snap log did **not** fire: Wine path is `S:\steamapps\common\Rabi-Ribi\`, no `"snap"` substring.

Live module list: `dxgi.dll` 228 KB at `61000000` **left in the present**. `lsteamclient.dll` / `steamclient.dll` / `steam_api.dll` / `steam.dll` held.

## Death: linux **57984**, first restore hung at `resume: releasing`

CONTINUE beach. MemAvailable 3.8 Gi → 2.5 Gi in-game. KEY_1 lived: mapid 1, **172** regions (53818 was 173; dxgi no longer captured), 520.6 MB, 48 threads, partition consistent. Compact 1 header / 0.00 MB. Wrapper heap `0BFE0000`. rss 525900 kB.

Copy lived: player back x=25914 y=8137, 165 restored, **1 skipped**, 34 contexts read back exactly. Then:

```
resume: releasing 48 thread(s)
fault: C000001D at 7BEA6B40 in kernel32.dll+16B40, thread 492, 0 frame(s) since the last restore
  eax=78CA0000 ... first word 00905A4D (MZ)
  stack-scan GUESS 78CAFACD  dinput.dll+FACD
exitpath: NtTerminateProcess called from ntdll.dll+45622
mixer/ucrt NtTerminateProcess - parking tid 492
```

No `resume: done`. No `exitpath: ... not parking`. Helper tid 504 was **not** parked. KEY_2 timed out, retry missed. Fail shot is the last presented beach frame (HP 90, same as the post-save shot). Pid killed.

Tid 492 is the **dinput** thread. Title save: `those threads belong to: dinput.dll x1, d3d11.dll x1`; park site `thread 492 at win32u.dll`. Beach KEY_1: `dinput.dll x1, d3d11.dll x13`. `C000001D` is still Wine “Exception in Unix call” after restore. Unparking the helper let ResumeThread hit it on dinput instead. Mixer-parking that tid still freezes the snap window.

172 vs 173 regions, 165 restored / 1 skipped vs 167 / 0: holding dxgi. Not the leave.

## Protocol

0 of 5 runs of 1+10. First restore did not finish. Mutation reload-then-save not reached. Heap bound: compact 1 header / 0.00 MB at first save, not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable 2.5 Gi in-game, 4.1 Gi after kill. No UI lock from memory pressure. Steam snap pid 8424 left running.

## Next lever

Do not park winevulkan. Do not Flush then rewind. Do not wait for Present after `request()`. Do not rewind unnamed `001D0000`. Do not capture held Wine handle pages. Do not park the savestate helper (closed as the freeze: 57984 helper lived; dinput took the park). Do not Flush after resume. Do not skip DXVK as the leave.

Snap unix after restore is still `C000001D`. Parking **any** waiter (helper, dinput, mixer) freezes the snap window so Ubuntu says Steam is not responding. Unmasking the leave (do not mixer-park dinput/win32u on that `C000001D` either) would look like 43397: process actually gone. The unix source is snap confinement, not a Steam overlay crash reporter. `g_game_dir` is the Wine `S:\` path, so a `"snap"` substring log will never see it.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-snapseam.txt`, `bound-save10-snapseam.out`, shots `snapseam-beach` / `bound-r1-after-first-save` / `bound-r1-load-1-fail`.
Laptop clone `f04e7e3` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` was `700965f3b88cad2fb6e997f13e8661f4`. Game left gone. Steam snap still up.
