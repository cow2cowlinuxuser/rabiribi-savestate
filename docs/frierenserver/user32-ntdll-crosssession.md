# USER32 / ntdll cross-session death on Wine

Belongs-elsewhere skip only let the copy *reach* this. It did not cause it.

After the skip, same-boot A→B on Proton:

- copy ran (`131 restored, 0 skipped`)
- `resume: done`
- heap 00150000 failed validation
- `C0000005` at `00000000`, verdict **call through a bad function pointer** from `USER32.dll+A5E8`
- follow-up write to NULL in `ntdll.dll+47595`
- 0 frames since restore; linux pid left as a zombie; wineserver still up

That is the Windows GDI/USER32 straddle on Wine: a NULL WNDPROC in `_WINPROC_wrapper`, not a Proton region-copier bug. The window object lives in user32/win32u in the present; the game's restored memory still holds pointers and procedure values from process A's HWND. Rewinding USER32 is the thing this engine does not do, on purpose, for the same reason it does not rewind GDI on Windows.

What this sitting ruled out:

- "the skip itself is the death" — without the skip, the load refuses and B stays up. With the skip, the copy reaches the USER32 call. The skip is a flashlight, not the fault.
- "no flushed fault means unknown" — this run flushed VEH. The address is NULL, the caller is USER32.
- "fix it by restoring the belongs-elsewhere bytes" — those regions were not covered by the live allocation; writing them would paint process B's objects with A's image.

Do not "fix" a Linux refuse or this USER32 death by rewinding GDI or parking a host GPU. The Windows same-boot `xrestore` path already completed a copy then died in `gdi32full` with `VIDEO_ENGINE_TIMEOUT_DETECTED`; that path stays gated.
