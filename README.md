# rabiribi-savestate

Does state save for Rabi-Ribi. Now usable within a session.

In-process save and restore of a running 32-bit Windows game: memory regions,
heap blocks, thread contexts, the clock, and audio cursors, taken and put back
without a debugger and without the game's cooperation. It renders on the GPU
and covers the monitor while doing it.

## Current state (v1.3)

Save and restore both complete in under a second and survive normal play.
Multiple full sessions with repeated saves and restores across rooms, with no
crash attributable to the engine. This is a large change from earlier versions,
where a restore usually faulted within a frame.

Three things got it there.

**The game's own heap.** `D3D9SW_GAMEHEAP=1` creates a heap for the game and
redirects the executable's `malloc` family into it. Rabi-Ribi links ucrtbase,
which does not create a heap — it calls `GetProcessHeap` and allocates
alongside ntdll, GDI and combase. Interleaving the game's blocks with Windows'
in one arena is what forced every restore to guess which blocks it owned, and
was behind most of the faults.

**Not reading device memory.** The region walk now skips `PAGE_WRITECOMBINE`
and `PAGE_NOCACHE` pages. Reading the aperture at 26 MB/s was the original
cause of the save-time crashes and of multi-second saves.

**Writing only what changed.** `D3D9SW_DIFFWRITE=1` compares before writing.
About 90% of a 390 MB snapshot is static DxLib arenas, so a restore writes
roughly 30 MB.

Rendering moved to the adapter in the same period: heavy scenes went from 19
fps to a stable 60, and mirroring textures into video memory cut the CPU-side
texture payload from 641 MB to about 195 MB, which is what makes a snapshot fit
in a 2 GB process at all.

`D3D11SW_BORDERLESS=1` covers the monitor at its true resolution and does the
upscale itself, so `D3D11SW_SCALE=point,integer` gives clean nearest-neighbour
pixels rather than the panel's scaler.

### Refused on purpose

Restoring a save taken in a different world is rejected. It faults immediately
on a pointer into what is now text data: the heap reuses blocks across a world
transition, and a restored pointer to a block that has since been freed and
reallocated cannot be made valid by writing memory back. `D3D9SW_CROSSWORLD=1`
allows the attempt for anyone experimenting.

Cross-session restore fails for the same underlying reason. The snapshot is
address-dependent and the game's dynamic allocations do not land at
reproducible addresses across launches. Both need a save that can be
deterministically repaired at restore time rather than replayed verbatim.

One concrete cause of the divergence has a name now. Two runs that should have
allocated identically split at a single `calloc` asking for 0x25 bytes in one and
0x2E in the other, and that block is a Vorbis vendor string: the game's music was
not all encoded with the same libVorbis version, so which track loads decides the
shape of the allocation stream. That makes the divergence game state, reproducible
given the same route, rather than a seed to be pinned.
[docs/audio_and_archive.md](docs/audio_and_archive.md) has the archive format, the
nine tracks responsible, and the `D3D9SW_GHVORBIS` knob that reports them live.

### Known gaps, measured rather than assumed

- Ribbon sweeps to the player after a restore. Both entities restore to correct
  positions, so she is chasing a third piece of state that is not in the
  snapshot.
- roughly 4.7 MB of `MEM_MAPPED` regions are never captured, because the region
  filter accepts `MEM_PRIVATE` and `MEM_IMAGE` only
- a heap can be classified as the game's and still contribute almost nothing,
  because heap extent is found by walking segment headers and much of a grown
  heap lives in reservations that do not carry one
- the snapshot is not a single instant; one region has been observed changing
  during the copy with every thread in the process suspended
- Steam's threads are not rewound, so alt-tabbing after a restore has been seen
  to fault inside `steamclient.dll`
- occasional texture atlas seams, and brief audio artefacts on the title screen

## Layout

- `savestate.c` / `savestate.h` — the engine. Large, and commented at length,
  usually with the failure that motivated each piece.
- `dsoundhook.c` — DirectSound buffer tracking, so play cursors can be put back.
- `d3d11_sw.c`, `swrast.c`, `dxbc.c`, `vsinterp.c` — the software renderer this
  grew inside. The game loads it as `d3d11.dll` / `dxgi.dll`.
- `gpu.c`, `gpu_quad.hlsl` — the hardware backend. Textures are mirrored into
  video memory and their CPU copies released where it is safe, which is what
  frees the address space a snapshot needs.
- `gameheap.c` — the game's private heap and the import redirection into it.
- `ss_harness.c`, `ds_harness.c`, `rr_harness.c` — drive the engine with no game
  attached. `rr_harness` is the most useful: it reproduces the game's structural
  shape (shared process heap, present-time system threads, tracked audio
  buffers, thread counts) and asserts oracles after every restore.
- `gh_replay.c` — replays a recorded allocation trace against candidate
  allocators, to ask which of them place blocks reproducibly.
- `dispprobe.c` — prints what every monitor-size API reports, side by side.
  Built without a DPI manifest on purpose, so it sees the invented coordinates
  the game sees.
- `tools/` — offline analysis, none of which needs the game running. `kanobi.py`
  reads the asset archive, `tagdiff.py` and `trdiff.py` compare two allocation
  traces by block identity rather than by address, `symres.c` turns the
  `module+RVA` in a fault report into a function and source line.
  `linux-rabi.sh` / `xrestore.sh` / `wrapper.sh` are the Linux restore path
  (Proton + the Steam game, not distro Wine + a no-game harness). They still
  compile with `zig cc -target x86-windows-gnu`; they do not add a Wine/MinGW
  toolchain. Launch always passes `-noaudio`.
- `examples/rabiribi/` — the configuration the game is actually run with.

## Building

Needs [zig](https://ziglang.org) as the C compiler.

```
./build.ps1
```

`build.ps1` also deploys to hardcoded local paths for other titles. Trim it.

## Linux (Proton)

This is the Linux restore path going forward: Proton Experimental, the Steam
copy of Rabi-Ribi, and `tools/linux-rabi.sh`. Distro Wine plus a no-game
harness is a separate Cloud Agent environment; it does not run the game.

In-session save and restore work under Proton, including room to room in the
same world. F5 saves, Shift+F5 loads. First launch is language select; later
launches go straight to the title. Launch always passes `-noaudio`.

```
tools/linux-rabi.sh build
tools/linux-rabi.sh deploy
tools/linux-rabi.sh launch
```

`xrestore` always sets `D3D11SW_GPU=0`. The launch line is
`steam-launch-wrapper` + reaper + SteamLinuxRuntime; bare
`proton waitforexitandrun` exits 53 while Steam holds the app. Native
DllOverrides in the Proton prefix are what make the game-folder `d3d11.dll`
win over DXVK. Wine/Proton is detected at save time (`wine_get_version`):
only game threads are frozen, HeapWalk is skipped, and xa2/gpu park is
skipped so the helper does not wait on wineserver.

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
