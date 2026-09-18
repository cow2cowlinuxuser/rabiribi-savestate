# frierenserver same-boot A→B (belongs-elsewhere refuse)

**Refuse.** Same-boot A→B copied nothing. B is still in-world on the beach.

```
 region 09281000+ff000 is excluded now but was saved
 region 09482000+fe000 now belongs elsewhere: alloc 09380000 vs 09480000, type 20000 vs 20000
 1 region(s) skipped because they are excluded now - live stacks or TEBs …
load: refused, 1 of 142 regions unrestorable
```

EXCLSKIP ate the TEB at `09281000`. The refuse is the old belongs-elsewhere veto on `09482000` (alloc `09380000` vs `09480000`). No `resume: done`. No vanish. Linux pid **9839** kept presenting (~10 fps). Steam never wrote `Game process removed` for 9839. Proton `err+seh` has no `C0000005`. Slot md5 still `3885930bd4872c671e3d448ead127cbd`.

This sitting did **not** reproduce the pre-reboot copy-then-gone. Same boot was not enough for the addresses to line up.

A (linux **8224**, wine 312) CONTINUE AUTO SAVE → beach, XTEST `1` saved 142 regions / 384.4 MB / mapid 1 at `x=26087 y=8213`. Steam removed A at 19:28:36 with exit -1 because we SIGTERM'd it.

Every miss is in the pack, not treated as a stop: ZZZ during NOW LOADING, AUTO SAVE click 32%×42% hitting NEW GAME, `pgrep -f` false-alive after A's SIGTERM, B's first AUTO SAVE click, first two `KEY_2` pulses with no sslog growth, empty `dmesg` (`Operation not permitted`; `journalctl -k` used instead).

Knobs still `SAVE_AT=0` `LOAD_AT=0` `QUIT_AT=0`. Evidence originally at `/home/fernserver/Downloads/frierenserver-crosssession-sameboot/`.
