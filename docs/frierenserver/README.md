# frierenserver Proton sittings

Index of sitting notes from the Lenovo IdeaPad (`frierenserver`, user `fernserver`, Ubuntu 24.04, snap Steam, Proton Experimental 11.0-100). They record what happened when save/restore was driven on the real game, not a Cloud VM.

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

Related: `docs/windows_savestate_ownership.md`.
