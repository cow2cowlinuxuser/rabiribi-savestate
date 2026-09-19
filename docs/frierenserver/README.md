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
| [frierenserver-gpu-insession-exitpath.md](frierenserver-gpu-insession-exitpath.md) | Arm RtlExitUserProcess/NtTerminateProcess at FREEZE=0; park mixer; 275+ loads on linux 118091 still alive (new record) |
| [code-vs-game-boundary.md](code-vs-game-boundary.md) | Three agents: wrappers, game, Wine. Wrappers remain the artifact; docs describe the seam |

Related: `docs/windows_savestate_ownership.md`.
