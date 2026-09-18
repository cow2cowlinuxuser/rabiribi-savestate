# frierenserver PARTHOLD sitting (2026-09-17)

PARTHOLD + `SAVE_AT=0` means-never went onto the wrappers already in the game folder. Z Z Z → map 3.

1. In-world save/restore **lived** (pid 63539, PARTHOLD skipped the two leaked heaps, player back in world 3, presents kept coming). The previous night's `ntdll+50260` did not return.
2. Same process **save→restore→save→restore** **lived**. Second save rewrote the slot (497.7 → 502.7 MB).
3. Same-boot cross-session: process B loaded A's slot and **refused** — `load: refused, 2 of 158 regions unrestorable` (excluded-now stacks/TEBs). B stayed up at ~22 fps. Stopped there.

DLLs still in the game dir. `SAVE_AT=0` `LOAD_AT=0`.

This is steps 1–3 of the held-heap plan: PARTHOLD is sufficient for in-session in-world restore on this box. Cross-session then names a different veto (live stacks/TEBs at addresses that held ordinary memory at the save), which is EXCLSKIP's job, not a new heap tear.

Evidence originally at `/home/fernserver/Downloads/frierenserver-parthold/` and `/home/fernserver/Downloads/frierenserver-parthold-logs-shots.zip`.
