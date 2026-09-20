---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Bound the wrapper heap (REWIND_SWHEAP=0)

GPU=3 / TEX_SCALE=1 in-session copy already works. The 7.1 Gi laptop died
because wrapper heap `0C010000` (held, never rewound) grew 2 MB → 490 MB
over 845 loads, then ~594 MB by save 318. Held-hash stayed UNCHANGED:
growth is between copies, not a bad restore.

## Cause

`D3D11SW_RETIRE_OBJECTS` (default on) never `free`s a COM header after
Release, so a restore can still find a vtable. Rabi-Ribi creates and
destroys ~3.5 textures a frame. At 60 fps that is unbounded headers on
the private wrapper heap (`calloc` → `sw_calloc` via `swalloc.h`).
Payload retain already skipped post-save objects (`born_gen >=
g_save_gen`); the header path did not.

The 3-hour stall on load 845 is still a wineserver / parked-mixer
question. Game threads were frozen for that copy, so the stall itself
did not create textures. It may still have made RSS worse by leaving
the already-grown heap sitting while PipeWire/Steam decayed, but it is
not the leak.

## Change

Same line as payload retain: if the object was born after the current
save, free the header. Snapshots cannot name it. `HeapCompact` after
`d3d11sw_retain_flush` (new save) and after `ledger_reap` (restore),
because `HeapFree` may not decommit. Pre-save retired headers still
stay until a stale Release needs them.

`tools/noaslr.py` is the Linux pin that `linux-rabi.sh build` already
called. `tools/noaslr.py` was missing on this clone, so the compile
succeeded and the pin step exited 2.

## What this is not

Not a stretch. Do not launch GPU=3 until this DLL is in the snap game
dir and a **short** sitting shows `held hash ... heap 0C010000` staying
near its first-save size across tens of pairs. Copy `d3d11.dll` by
hand: `linux-rabi.sh deploy` still resets `GPU=0`.

Live post-save objects the game still held at restore remain on
`g_res_head` (ledger_reap will not pull payloads out from under them).
That is a smaller leak than retired-header churn. If the short sitting
still climbs, that walk is the next candidate.

Mixer/wineserver stall at load 845 is still open, after RAM.
