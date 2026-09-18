# frierenserver cross-session resume (after reboot)

A's slot was still `7ece25c853fd3d09433d5d634edc6768`, so this was B CONTINUE → load only. After reboot that is a **cross-boot** load, not same-boot.

**B lives. Copy never starts.** Not freeze, not process-gone, not a wineserver hang.

evemu `KEY_2` (XTEST `2` only drew a pink "2" in-world) refused the load: `load: refused, 4 of 142 regions unrestorable`. Four **belongs elsewhere** alloc-base mismatches; EXCLSKIP skipped 3 TEBs. linux **6127** kept presenting, wineserver **6037** and Steam reaper still held it, no `Game process removed`. Same old `savestate.c` VirtualQuery veto, not a new skip.

A previous "freeze" with `WINEDEBUG=+seh` was a **trace**: d3d11 capped at 20k lines, **0.2 fps**, process still running, no B load line. This sitting used `err+seh` only.

Same-boot copy-then-vanish (`resume: done`, Steam exit -1, no `C0000005`) was not reproduced: that needs A and B on the same boot.

Evidence originally at `/home/fernserver/Downloads/frierenserver-crosssession-resume.md`.
