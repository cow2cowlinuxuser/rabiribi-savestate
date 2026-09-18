# frierenserver in-world restore on main (2026-09-17)

Stayed on `origin/main` `4a9fa0d`. Z Z Z from PRESS START into the starting map (`mapid 3`).

Save worked (498.7 MB). Restore copied (`player IS back … world 3`, `resume: done`) then died immediately: `C0000005` at `ntdll.dll+50260`, 0 frames.

That is the same site as the cloud Proton VM **before** PARTHOLD (`savestate: do not rewind half of a heap we are holding`, PR #3 `768b9ec`). Title restore on this box earlier still counts as lived. No leftover-Left walk (process was gone). Wrappers left in the game folder.

Evidence originally at `/home/fernserver/Downloads/frierenserver-proton-inworld/` (`logs/`, `shots/`) and `/home/fernserver/Downloads/frierenserver-proton-inworld-logs-shots.zip`.
