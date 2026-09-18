# frierenserver cross-session retry (EXCLSKIP on)

B **dies after the copy**. It does not live in world 3. There is **no flushed fault address**.

In-world A (linux **80768**, wine **316**) saved mapid **1** / world **1** (Rabi Rabi Beach, `x=25999 y=8169`): 143 regions, 385.3 MB, slot md5 `ad6d7b6091d878dfc40a048e64a3ea52`. B (linux **84028**, wine **320**) loaded that slot with `EXCLSKIP=1` already on. Three excl-now TEBs were skipped, belongs-elsewhere did **not** fire, copy ran (`133 restored, 0 skipped`), `resume: done`, then the process was gone. Steam dropped pid 84028 at `16:05:07` with **exit code -1**. Last wrapper lines: `savestate restored slot 0 in 655.4 ms` and present pacing 833 ms behind. No `C0000005` in savestate/d3d11, no dmesg segfault.

World 3 was gone on this retry: NEW GAME overwrote `save/save0.sav` at 16:03, so ZZZ lands on the beach.

Earlier same-boot file-select A→B (attempt 1) still **refuses** with EXCLSKIP on: `load: refused, 3 of 142 regions unrestorable`, all three `belongs elsewhere` (`09482000` alloc `09380000` vs `09480000`, plus two 36 MB regions). That is the old VirtualQuery alloc-base/type mismatch, not the EXCLSKIP class.

Knobs: `SAVE_AT=0` `LOAD_AT=0` `QUIT_AT=0`; `EXCLSKIP=1` `PARTHOLD=1` `GPU=0`.

Evidence originally at `/home/fernserver/Downloads/frierenserver-crosssession-retry.zip`.
