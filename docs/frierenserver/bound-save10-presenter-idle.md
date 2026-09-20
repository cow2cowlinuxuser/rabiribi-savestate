---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Presenter idle cannot ack: that thread is already in request()

Iterate-until-die after [bound-save10-adapter-flush.md](bound-save10-adapter-flush.md). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement. Four wrapper changes, four deaths. The named seam is not wineserver Sleep vs Present. It is `request()`.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. Frozen windows were killed by exact pid so the laptop UI was not left locked. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run.

## Door (26458)

Do not park winevulkan (26458 left instead of freezing Present). Do not Flush on the helper then rewind (20929 hung at `resume: releasing`; 24023 hung at `SetThreadContext` `C000001D`). Close load-time command buffers without a wineserver Flush on the rewind path.

Hypothesis: helper Flush races the recording game thread. After Present, CBs are closed; spin in user mode so `SuspendThread` is local. Helper waits for ack, then freeze.

## Death 1: linux **28336**, idle-after-Present, helper `Sleep(16)`

DLL md5 `8985816f2e8230fef622c42472f4601f` (`fdd3e47`). CONTINUE beach. KEY_1 lived: mapid 1, 178 regions, 522.0 MB, 48 threads.

First KEY_2:

```
wine: waiting for presenter to idle after Present (no Flush on the rewind path)
wine: presenter did not idle in 1440 ms - freezing anyway
```

Zero `gpu: wine idled after Present` in `d3d11_sw.log`. Copy lived (172 restored, player back x=25958 y=8149, contexts 34 set). Hung at `resume: releasing 48 thread(s)`. Same hang as 20929 without a load Flush. Pid killed.

Helper `Sleep(16)` is wineserver. Presenter was already in `Present` / `DwmFlush` (also wineserver). Idle never acked. Freeze-anyway while a thread is in a unix call hangs `ResumeThread`.

## Death 2: linux **31417**, user-mode wait, QPC every spin

DLL md5 `ecebc065b80a0dd92cb6b1c34cf92e40` (`fb4328c`). Replaced `Sleep` with `YieldProcessor` plus `QueryPerformanceCounter` every loop. Save Flush stayed.

KEY_1 lived. First KEY_2: presenter did not idle in **5000 ms**, freeze-anyway, copy lived, **`resume: done`**. Then:

```
exitpath: adapter RtlExitUserProcess - not parking the presenter
frame 0: winevulkan.dll+18E57
```

Process gone. Same door as 26458. QPC every spin is still wineserver; `DwmFlush` starved for the whole wait, so `gpu_frame_end` never saw `idle_req`. Removing Sleep was enough for `ResumeThread` to return. It was not enough to close CBs.

## Death 3: linux **33277**, hold after DwmFlush, idle before xa2 park

DLL md5 `3c835539019810cf12fc8a048b6452de` (`4943c88`). Sample QPC only every 65536 pauses. Idle wait before `xa2_sw_park` / `dsh_quiet`. `gpu_wine_idle_hold` after Present and after `DwmFlush`.

Still: presenter did not idle in 5000 ms. `resume: done`. Adapter `RtlExitUserProcess`. Process gone. KEY_2 never started load 2.

The hold after `DwmFlush` never ran because the Present thread is not presenting.

## The seam: `request()` owns the presenter

`savestate_load` → `request(REQ_LOAD)` runs on the thread that polled KEY_2, which is the Present thread (`Swap_Present` / `gpu_frame_end`):

```
InterlockedExchange(&g_ctl->busy, 1);
InterlockedExchange(&g_ctl->request, req);
while (InterlockedCompareExchange(&g_ctl->busy, 0, 0))
    YieldProcessor();
```

That thread spins in user mode until the helper finishes `do_load`. The helper waits for that same thread to reach `gpu_frame_end` / idle-hold. It never will. The idle handshake cannot ack.

Save already works this way: the presenter is in `request()` while the helper Flushes. Save Flush is from the helper with the presenter in a user-mode spin, not mid-Present. Load Flush from the helper with the same spin still deadlocks rewind (20929 / 24023). The difference is rewind, not which thread is presenting.

## Death 4: linux **35034**, Flush on the presenter before `request()`

DLL md5 `db669facfc354b5d07a5677d6464e987` (`8dda2a2`). Close CBs on the Present thread, then `request()`. Helper does not idle-wait, does not Flush.

`d3d11_sw.log`:

```
gpu: wine Flush on the presenter before request() so load freeze is not a helper Flush
gpu: wine presenter Flush returned
```

sslog:

```
wine: not waiting for presenter idle - that thread is already in request(); CBs closed before the spin
wine: parking software xa2 mixer, skipping waveOutPause (wineserver)
wine: froze 35 game/mixer thread(s), left 13 wineserver-side thread(s) running
WARNING: thread 480 - could not read its context back, so whether the registers took is unknown (err 3221225501)
```

`3221225501` is `C000001D`. sslog stuck at line 483. Never `load: slot`. Never `resume: done`. Same as **24023**, now from the presenter thread. Pid killed.

**Flush then rewind is a dead end no matter which thread calls Flush.** DXVK workers are in unix calls. `SetThreadContext` / `ResumeThread` waits on wineserver. Drain (helper `Sleep(200)`, presenter 4e6 `YieldProcessor`) does not help.

## Protocol

1 save + 1 restore of the 1+10×5, four times, four deaths. Mutation reload-then-save not reached. Wrapper heap not measured across tens of pairs. Compact still 1 header / 0.00 MB at first save. Not cross-session. USER32 not rewound. Exe not dumped.

MemAvailable stayed above 2.4 Gi until pids were killed. No UI lock from memory pressure.

## Next lever

Without Flush, first restore lives (`resume: done`, player back) then `winevulkan+18E57` `vkEndCommandBuffer` abort, process allowed to leave (26458 / 31417 / 33277). With Flush, rewind never finishes (20929 / 24023 / 35034).

Do not park that tid. Do not Flush before rewind. Do not wait for the presenter to Present after `request()` — it is already the waiter.

Close CBs some other way that does not put DXVK into unix calls across freeze, or stop rewinding the object `vkEndCommandBuffer` still holds. Related warning, unchanged: `system thread 524 holds 001DCA88` on unnamed heap `001D0000`, “which we rewind”.

## Left on disk

`Downloads/frierenserver-gpu-insession/bound-save10.sh`,
logs `sslog-bound-save10-idle.txt` / `-umwait.txt` / `-hold.txt` / `-precb.txt`,
shots `idle-r1-load-1-hung.png`, `bound-r1-after-first-save.png`, `precb-r1-load-1-hung.png`.
Laptop clone commits `fdd3e47` `fb4328c` `4943c88` `8dda2a2` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Live `d3d11.dll` in the snap game dir is `8dda2a2` (`db669facfc354b5d07a5677d6464e987`). Game left gone.
