---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver commit-hold: two in-session pairs (2026-09-18)

Stupid idea, measured. Fill the laptop's host commit, **hold it**, then save/restore
twice in different in-world states. Diff the slot `.regions` files (text copies —
that does not allocate inside the game). Wrappers stayed the artifact; the filler
was a sibling Linux process, not a second tool shipped next to the exe.

`steam://rungameid/400910` only. CONTINUE into Rabi Rabi Beach (mapid 1).
`PARTHOLD=1` `EXCLSKIP=1` `GPU=0` `SAVE_AT=0`. linux **73913**. Filler
**74687** held **960 MB** RSS (`MemAvailable` floor 1400 MB) across both pairs.

Steam Cloud Conflict blocked the first launch; Local Save + Continue, then the
game came up.

## Named outcomes

**1. Host commit fill does not freeze the game's allocated-section set.**
Pair 1 save: **139** regions, 384.8 MB, player `x=25914 y=8137`.
Pair 2 save (after walking): **143** regions, 388.2 MB, player `x=26072 y=8209`.
Same `(base,size,prot)`: **138**. Delta: `10CE0000` grew `1C0000 → 200000`, plus
four new regions at `161D1000`–`164D0000`. VERDICT: **CHANGED**.

The 32-bit process still had VA. Filling laptop RAM/swap does not force Wine
`VirtualAlloc` to fail, and it does not pin the savestate region list.

**2. In-session restore still lives under that pressure.**
Pair 1 load: `133 restored`, `resume: done`. Pair 2 load: `136 restored`,
`resume: done`. No `C0000005`. Game still presenting after both. Held-heap hashes
UNCHANGED across the copies (process heap + wrapper heap) — same as the MesHook
sitting; the copy is not painting those holds.

**3. Measurement that does not break the charge:** copy `.regions` and
`/proc/pid/maps`. Do not copy the 400 MB `.bin` during the hold. Linux map line
count moved 765 (prefill, in-world) → 793 (after save 1) → 799 → 805 (after
restore 2) even while the filler stayed mapped.

## What this is not

Not a way to coerce the game into a compliant island by starving the host.
The 138 shared regions are the repeatable core of *this* session; the extra four
are ordinary play allocating. Cross-session USER32/HWND is unchanged by this.

## Left on disk

`/home/fernserver/Downloads/frierenserver-commit-hold/` (filler source, pair
indexes, maps, shots, sslog). Game left running. Filler released after the diff.
