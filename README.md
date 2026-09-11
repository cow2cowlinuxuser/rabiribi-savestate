# rabiribi-savestate

Does state save for Rabi-Ribi. Not complete, and not reliable.

In-process save and restore of a running 32-bit Windows game: memory regions,
heap blocks, thread contexts, the clock, and DirectSound cursors, taken and put
back without a debugger and without the game's cooperation. It works often
enough to be interesting and not often enough to be used.

## Current state

A save and a restore complete. The engine's own checks pass: every captured
region is written back byte-for-byte (`0 WRONG`), no region is skipped, and all
tracked DirectSound buffers are stopped and seeked. The game frequently faults
within a frame of the resume anyway.

The best current account of why is a split rather than a copying failure. The
game's runtime allocates from the shared process heap, so one heap serves both
it and Windows. Block-level restore with ownership filtering deliberately
leaves any block Windows can also reach in the present, and about twelve system
threads keep running forward with stacks that are never rewound. Objects that
those threads hold then no longer describe the buffers the restored game
believes in.

### What changed (2026-09)

**Class B TEB veto (the deferred win).** System thread TEBs hold ~8 blocks on
the game's rewound heap every session. Those TEBs are not in the snapshot but
the blocks are, so the blocks were being written back while the system threads
that held them kept running. `blk_sys_mark` now scans every system thread's
full TEB as a veto root for the block-ownership closure, so those blocks are no
longer restored. See `restore_invariants.md` invariant 6.

**Thread-set invariant.** The restore already compared thread sets and logged
divergences, but the counts were buried in the log and no harness checked them.
`savestate_thread_set()` now exports the fresh / recycled / gone counts, and
both harnesses (`rr_harness`, `ss_harness`) report them after every restore.
See `restore_invariants.md` invariant 4.

Both changes need a Windows game run to verify the death rate. The harnesses
exercise the code paths and log the counts, but the harness's thread set is
smaller and more stable than the game's.

Known gaps, all measured rather than assumed:

- roughly 4.7 MB of `MEM_MAPPED` regions are never captured, because the region
  filter accepts `MEM_PRIVATE` and `MEM_IMAGE` only
- a heap can be classified as the game's and still contribute almost nothing,
  because heap extent is found by walking segment headers and much of a grown
  heap lives in reservations that do not carry one
- the snapshot is not a single instant; one region has been observed changing
  during the copy with every thread in the process suspended

## Layout

- `savestate.c` / `savestate.h` — the engine. Large, and commented at length,
  usually with the failure that motivated each piece.
- `dsoundhook.c` — DirectSound buffer tracking, so play cursors can be put back.
- `d3d11_sw.c`, `swrast.c`, `dxbc.c`, `vsinterp.c` — the software renderer this
  grew inside. The game loads it as `d3d11.dll` / `dxgi.dll`.
- `ss_harness.c`, `ds_harness.c`, `rr_harness.c` — drive the engine with no game
  attached. `rr_harness` is the most useful: it reproduces the game's structural
  shape (shared process heap, present-time system threads, tracked audio
  buffers, thread counts) and asserts oracles after every restore.
- `examples/rabiribi/` — the configuration the game is actually run with.

## Building

Needs [zig](https://ziglang.org) as the C compiler.

```
./build.ps1
```

`build.ps1` also deploys to hardcoded local paths for other titles. Trim it.

## Running the harness

```
./rr_harness32.exe [cycles] [restores] [call-anyway]
```

`call-anyway=1` dereferences a callback pointer the oracle has already declared
bad, which is what the game does and how it dies. Off by default so a run
produces counts instead of one corpse.

Settings are read from `d3d9_sw.cfg` beside the executable, and from the
environment, which wins. Both are logged with their source at startup.

## Warning

The engine patches instructions and hooks imports in the host process. Those
patches are gated on the executable being `rabiribi.exe`, because they were not
always, and a test harness that happened to contain a matching nine-byte
sequence had a branch rewritten under it.
