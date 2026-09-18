# frierenserver belongs-elsewhere skip (same-boot A→B copy)

**Die after `resume: done`.** The belongs-elsewhere refuse no longer blocks the copy. Same-boot A→B copied, then B died at 0 frames.

```
 region 09782000+fe000 now belongs elsewhere: alloc 09680000 vs 09780000
 region 15BC1000+ff000 now belongs elsewhere: alloc 15AF0000 vs 15BC0000
  2 region(s) skipped because they now belong elsewhere …
load: slot 0, 131 restored, 0 skipped …
 resume: done, all threads runnable
 heap check: heap 00150000 … FAILED validation after the restore
fault: C0000005 at 00000000 in unknown+0, thread 320, 0 frame(s) since the last restore
 VERDICT: call through a bad function pointer … from USER32.dll+A5E8
fault: C0000005 at 7BF57595 in ntdll.dll+47595 … WROTE to 00000000
```

linux **15103** is a zombie. wineserver **15017** still up. Steam never wrote `Game process removed` for 15103. Presents stuck at 532. That is the old vanish, this time with a flushed VEH.

`savestate.c`: `region_still_ours` — if the live allocation still covers the saved range, restore in place; otherwise skip that region (same belongs-elsewhere class, not a new veto) and copy the rest. Also skip excluded-now regions in the write loop. Cherry-picked PARTHOLD + `SAVE_AT=0` means-never onto `main` `4a9fa0d`, then this skip.

Neither belongs-elsewhere region was fully covered, so both were skipped, not restored in place.

`SAVE_AT`/`LOAD_AT`/`QUIT_AT` still 0.

See [user32-ntdll-crosssession.md](user32-ntdll-crosssession.md) for why the copy-then-death is not a Proton copier bug.

Evidence originally at `/home/fernserver/Downloads/frierenserver-crosssession-belong/`.
