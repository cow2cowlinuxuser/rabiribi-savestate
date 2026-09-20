# GPU in-session save/restore

Goal: Proton adapter-path save/restore (`D3D11SW_GPU=3`, `TEX_SCALE=1`) that survives as many in-session save/load cycles as the process will take. Simple movement away and let the game happen — do not play. No cross-session.

A 20-minute wake loop on the Project re-queues [GPU in-session restore](bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d) when that child is idle. Catt Man stops the loop.

## What already failed

- `D3D11SW_GPU=1/2/3`: copy completes, then `C0000005` at `ntdll.dll+5025B`, winevulkan on the stack, a few frames after `resume: done`.
- `D3D11SW_GPU=3` is the knob that actually draws on the adapter (textures leave the process). `D3D11SW_GPU_HALFPIXEL` is a separate sampling look.
- GPU=0 + PARTHOLD still the path that lived.
- `D3D11SW_TEX_SCALE=1` on the Wine-heap-hold DLL: copy completes, then `C0000005` at `d3d11.dll+C617` (`cmp [esi+0x10],1`) with `esi` in the reserved software arena, 1 frame after `resume: done`.
- Same process, later load: `ntdll.dll+5025B` writing `3F800004` (float `1.0`) into software arena `20000000` during wrapper `HeapAlloc`. Process heap failed validation from pair 2. Unnamed Wine heaps were UNCHANGED.
- Wrapper-span hold (`1fa9e71`) on linux **91172**: loads 1–44 lived, load 45 `ExitProcess` from `ucrtbase.dll` at `7B7A5B5C`, 198 frames after load 44, no exception. Process heap hash stayed 64 KB (handle page only).
- Holding process-heap HeapAlloc spans / Wine header scan: snapshot 520→411 MB, first restore dies in `rabiribi.exe`. Those spans are the game's ucrt mallocs.
- linux **101830** (`37e870d`): same quiet `ExitProcess`, now stacked through **mmdevapi.dll**. Mixer thread still running; Wine GPU/xa2 park is a no-op. Faster cadence hit it at load 8.
- linux **104496** (`580e55e`): SPI already succeeds in 10144 bytes and still parses 0/49. Unknown-audio freeze of tid 500 (parked in mmdevapi user-mode) lived 27 loads, then mixer `ExitProcess` on resume.
- linux **108167** (`acda254`): ignoring mixer `ExitProcess` lived **55 loads**, then a third `ExitProcess` from the same `ucrtbase` caller with a short backtrace actually exited.
- linux **114521** (`4c25cf2`): ignoring every `ucrtbase` `ExitProcess` and returning retried **2146 times**, then `C0000005` in ucrtbase on the mixer tid. Dead before save 7.
- linux **116114** (`aa44a17`): park mixer with `Sleep(INFINITE)` — 9 loads, then quiet leave, **zero** `ExitProcess` IAT hits. `RtlExitUserProcess` int3 was gated on `FREEZE>=2`. UniqueThread+4 `9` is not a waiter.
- linux **11211**: heap-bound DLL, `FREEZE=0` did not arm exitpath; DxLib bound Wine `XAudio2_8`; first KEY_2 `ExitProcess` from ucrtbase/mmdevapi.
- linux **14079**: native `xaudio2_8.dll` attached `xa2_sw`; Wine `waveOut` still pulled mmdevapi; first KEY_2 hung (`C000001D`, parked mixer, no `resume: done`).
- linux **16955**: mix without Wine waveOut. **First restore lived** (`resume: done`, player back). Then `winevulkan+18E57` → `RtlExitUserProcess`; parking tid 524 froze Present. Sitting: [bound-save10-xa2-winevulkan.md](frierenserver/bound-save10-xa2-winevulkan.md)
- linux **20929**: Flush without Present on save **and** load. Save lived. First restore copied then hung at `resume: releasing`. Pid killed.
- linux **24023**: Flush plus 200 ms drain. First restore hung at `SetThreadContext` `C000001D`. Pid killed. Flush-then-rewind is a dead end.
- linux **26458**: Flush on save only; do not park winevulkan. **First restore lived**. Adapter `RtlExitUserProcess` allowed to leave; process gone (Present not frozen). MesHook `C000001D` on tid 524 during the real exit. Sitting: [bound-save10-adapter-flush.md](frierenserver/bound-save10-adapter-flush.md)
- linux **28336**: idle-after-Present, helper `Sleep(16)`. Idle never acked. Copy lived, hung at `resume: releasing`. Pid killed.
- linux **31417**: user-mode wait, QPC every spin. Idle never acked in 5s. `resume: done`, then winevulkan ExitProcess. Process gone.
- linux **33277**: hold after DwmFlush, idle before xa2 park. Still no ack. Same leave as 26458.
- linux **35034**: Flush on the Present thread before `request()`. Flush returned, hung at `SetThreadContext` `C000001D` (thread 480). Same as 24023. Sitting: [bound-save10-presenter-idle.md](frierenserver/bound-save10-presenter-idle.md). The Present thread is already spinning in `request()`; a helper idle wait can never see `gpu_frame_end`.

## Constraints

- Drop-in wrappers only (no second injector).
- Do not dump Steam-protected `rabiribi.exe`.
- Do not rewind USER32/GDI. Do not run `xrestore.ps1` on the Windows host.
- Leave wrappers in the snap Steam game dir.

## Done when

The loop is stopped, or GPU=3 / TEX_SCALE=1 in-session save/restore no longer dies across long same-process load counts.

**Met 2026-09-19 on frierenserver (TEX_SCALE=2):** three KEY_1 / KEY_2 pairs on linux **82152**, GPU=3, zero `fault:` lines. Unnamed Wine heaps (`001D0000`, `02070000`) stay in the present so winevulkan does not walk a rewound heap. Sitting: [frierenserver-gpu-insession.md](/cursor/stores/bc-8736ebbb-68f2-4433-8098-b515b0106832/docs/frierenserver/frierenserver-gpu-insession.md)

**Record (TEX_SCALE=1):** **845 loads** on linux **118091** (`bdb4ca7`), then the Steam snap scope tore down at 18:35 (quiet leave, no second `exit:`, no `fault:`). Mixer tid 504 parked at load 8. Load 845 stalled ~3 h between `save: slot` and `resume: done`. Sitting: [frierenserver-gpu-insession-exitpath.md](/cursor/stores/bc-8736ebbb-68f2-4433-8098-b515b0106832/docs/frierenserver/frierenserver-gpu-insession-exitpath.md)

Prior mixer/ucrt sittings: [frierenserver-gpu-insession-ucrt.md](/cursor/stores/bc-8736ebbb-68f2-4433-8098-b515b0106832/docs/frierenserver/frierenserver-gpu-insession-ucrt.md), [frierenserver-gpu-insession-mixer.md](/cursor/stores/bc-8736ebbb-68f2-4433-8098-b515b0106832/docs/frierenserver/frierenserver-gpu-insession-mixer.md)
