# frierenserver Proton sittings

Index of sitting notes from the Lenovo IdeaPad (`frierenserver`, user `fernserver`, Ubuntu 24.04, snap Steam, Proton Experimental 11.0-100). They record what happened when save/restore was driven on the real game, not a Cloud VM.

The shipped artifact is still the drop-in wrappers. These notes name the
seam (wrappers / game / Wine). They are not a second tool.

These files were first committed on the laptop clone as `e8bb9e1` (`docs: add frierenserver Proton A→B sitting notes`) on `cursor/belongs-elsewhere-coalesce-bd2d`. That clone lived at `/tmp/rabi-laptop/rabiribi-savestate` and `git push` failed there (no GitHub write creds). The notes below were recovered from the sitting reports of [Laptop Proton restore check](https://cursor.com/agents/bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d) so the measurements are not only in `/tmp`.

| Note | What it answers |
|---|---|
| [frierenserver-proton.md](frierenserver-proton.md) | Title save/restore on laptop `main`; `SAVE_AT=0` still meant frame 1 |
| [frierenserver-proton-inworld.md](frierenserver-proton-inworld.md) | In-world restore on `main` died at `ntdll+50260` (pre-PARTHOLD) |
| [frierenserver-parthold.md](frierenserver-parthold.md) | PARTHOLD: in-world and two pairs lived; same-boot A→B refused 2 excl-now regions |
| [frierenserver-exclskip.md](frierenserver-exclskip.md) | EXCLSKIP lets those TEB/stack regions skip instead of refusing |
| [frierenserver-crosssession-retry.md](frierenserver-crosssession-retry.md) | EXCLSKIP on: in-world A→B copied, then B vanished with no flushed fault |
| [frierenserver-crosssession-resume.md](frierenserver-crosssession-resume.md) | After reboot (cross-boot): B lives, copy never starts, belongs-elsewhere refuse |
| [frierenserver-crosssession-sameboot.md](frierenserver-crosssession-sameboot.md) | Same-boot refuse on one belongs-elsewhere region; B stayed in-world |
| [frierenserver-crosssession-belong.md](frierenserver-crosssession-belong.md) | Belongs-elsewhere skip: copy ran, then USER32/ntdll NULL call |
| [user32-ntdll-crosssession.md](user32-ntdll-crosssession.md) | Why that post-copy death is the Windows GDI/USER32 straddle on Wine |
| [frierenserver-heldhash-meshook.md](frierenserver-heldhash-meshook.md) | Held-heap hash UNCHANGED; MesHook refused and the game still runs; slotfile A→B still dies on a bad function pointer; CONTINUE+pos patch lives |
| [frierenserver-commit-hold.md](frierenserver-commit-hold.md) | Host commit fill (960 MB) does not pin the region list; two in-session pairs still restore (VERDICT CHANGED) |
| [frierenserver-commit-hold-gpu.md](frierenserver-commit-hold-gpu.md) | GPU ladder 0→3 under held host fill: GPU=2/3 draw on adapter; in-session restore dies C0000005 on every GPU>0 |
| [frierenserver-gpu-insession.md](frierenserver-gpu-insession.md) | GPU=3 in-session: unnamed Wine heaps held; three KEY_1/KEY_2 pairs lived on one process |
| [frierenserver-gpu-insession-noscale.md](frierenserver-gpu-insession-noscale.md) | GPU=3 TEX_SCALE=1: resource-list sanitize; three KEY_1/KEY_2 pairs lived |
| [frierenserver-gpu-insession-stretch.md](frierenserver-gpu-insession-stretch.md) | GPU=3 TEX_SCALE=1 stretch: 44 loads lived on 1fa9e71; load 45 ExitProcess from ucrtbase, no exception |
| [frierenserver-gpu-insession-heaps.md](frierenserver-gpu-insession-heaps.md) | Holding process-heap growth dies load 1; ExitProcess stack is mmdevapi |
| [frierenserver-gpu-insession-mixer.md](frierenserver-gpu-insession-mixer.md) | Mixer freeze: SPI already 10 KB and still 0/49; unknown-audio freeze + ignore mixer ExitProcess lived 55 loads (new record) |
| [frierenserver-gpu-insession-ucrt.md](frierenserver-gpu-insession-ucrt.md) | Returning from ucrtbase ExitProcess retries 2146× then C0000005; park sitting died load 9 with no ExitProcess IAT |
| [frierenserver-gpu-insession-exitpath.md](frierenserver-gpu-insession-exitpath.md) | Arm RtlExitUserProcess/NtTerminateProcess at FREEZE=0; park mixer; 845 loads on linux 118091 then Steam snap scope died |
| [stuck-at-208448.md](stuck-at-208448.md) | After 118091/845 Steam snap teardown: new sitting 208448 at 0 saves; waiting KEY_1 / first dsh_quiet |
| [cadence-save5-208448.md](cadence-save5-208448.md) | One save then five restores every five minutes on linux 208448 |
| [engine-vs-linux.md](engine-vs-linux.md) | Proton in-session copy is good (845 then 366 loads, 0 fault); Linux limiter is 7.1 Gi RAM / growing wrapper heap, not resume correctness |
| [wrapper-heap-bound.md](wrapper-heap-bound.md) | Free post-save retired COM headers; first candidate to keep wrapper heap 0C010000 flat |
| [bound-save10-xaudio2-8.md](bound-save10-xaudio2-8.md) | Bound sitting died load 1: Wine XAudio2_8/mmdevapi, xa2_sw never attached |
| [bound-save10-xa2-winevulkan.md](bound-save10-xa2-winevulkan.md) | Native XAudio2_8 attached; skip Wine waveOut; first restore lived; winevulkan ExitProcess parked presenter |
| [bound-save10-adapter-flush.md](bound-save10-adapter-flush.md) | Do not park winevulkan; Flush-then-rewind deadlocks wineserver; save-only Flush, first restore lived, presenter allowed to leave |
| [bound-save10-presenter-idle.md](bound-save10-presenter-idle.md) | Idle handshake cannot ack: Present thread is already in `request()`. Presenter Flush before `request()` is 24023 again |
| [bound-save10-unnamed-heaps.md](bound-save10-unnamed-heaps.md) | Unnamed heaps held: 001DCA88 warning gone; first restore lived; winevulkan+23434 still left via process-heap 0015D390 |
| [bound-save10-handlepage.md](bound-save10-handlepage.md) | Every held Wine handle page excluded; partition consistent; helper Sleep parked the waiter; YieldProcessor then process left |
| [bound-save10-cb-unix.md](bound-save10-cb-unix.md) | Flush after resume (helper 45466, Present 49235) parks helper; skip-DXVK 53818 still parks; title skip 52672 black window |
| [bound-save10-snap.md](bound-save10-snap.md) | Snap named: unparking helper/lsteamclient holds dxgi; first restore hangs at resume: releasing; mixer parks dinput tid 492 |
| [bound-save10-dinputunpark.md](bound-save10-dinputunpark.md) | Do not park dinput: restores 1–2 `resume: done`; process heap 00150000 FAILED; load 3 never logs |
| [bound-save10-heapvalskip.md](bound-save10-heapvalskip.md) | Skip HeapValidate of held Wine heaps; restore 1 hangs at ctx_verify GetThreadContext C000001D tid 376 |
| [code-vs-game-boundary.md](code-vs-game-boundary.md) | Three agents: wrappers, game, Wine. Wrappers remain the artifact; docs describe the seam |

Related: `docs/windows_savestate_ownership.md`.
