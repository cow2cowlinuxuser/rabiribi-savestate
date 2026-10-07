# Proposal: a self-describing slot manifest

The idea comes from Capcom's [REDox](https://github.com/CAPCOM-TD-OSS/REDox),
a .NET library for structured data. We can't use the library itself. Four of
its design ideas carry over:

- an index built when writing, so reading doesn't have to scan;
- `$id`/`$ref` references resolved once, against the whole document;
- an unchanged base with edits layered on top;
- parallel work above a size threshold.

The proposal: describe every save in a structured, versioned manifest (JSON),
keep the memory bytes in binary as now, and have every load step read its
decisions from the manifest.

## What a slot is today

| File | Content | Problem |
|---|---|---|
| `.bin` | Raw memory bytes, 1-2 GB | Fine as it is |
| `.meta` | A raw copy of the in-memory `Slot` struct, 1.95 MB | Tied to the struct's layout: any field change breaks every old save without warning. Unreadable without our code. |
| `.regions` | A text list of regions and modules | Readable, but nothing in the engine reads it back |
| `.cfg` | Knob values and wrapper build | Already a small manifest, and it has paid off |

The engine also works out the same facts several times, in different places,
in different ways. On 2026-10-02 that produced a real bug. The pre-freeze
stand-in count matched threads by entry point. The transplant matched by
entry point plus "owner" (the first module found on the stack). They
disagreed, so no stand-ins were started and 16 workers were restored with no
thread behind them. Nothing in the log flagged the disagreement.

## The manifest

`d3d9sw_slotN.json` is written at save time. It is JSON Lines: one object per
line, each with a `"k"` (kind) field. The reasons for that format:

- **Writing is easy.** It's a fixed buffer and `WriteFile`, with no
  allocation, which is safe while game threads are frozen.
- **Reading is easy.** A flat line parser is enough; nested JSON isn't needed.
- **It works with existing tools.** It diffs line by line, Python reads it in
  one line, and the `connect/` tooling can use it directly.

The values in this sample are illustrative:

```json
{"k":"header","v":1,"game":"launcher.exe","game_stamp":"59241F07","wrapper":"1221120-01DD52CBDE892C2C","pid":21512,"boot":"...","saved":"2026-10-02T20:28:31"}
{"k":"cfg","name":"D3D9SW_PHYSX","val":"1"}
{"k":"module","name":"haydee.dll","base":"6B1F0000","size":"...","stamp":"...","rewound":true}
{"k":"region","base":"20000000","size":"...","file_off":"...","prot":4,"kind":"heap","owner":"gameheap"}
{"k":"thread","id":"t7","tid":45920,"entry":"haydee.dll+2E1F70","owner":"PhysX3_x86.dll","stack":["0B210000","0B220000"],"role":"haydee.dll+2E1F70#5"}
{"k":"ref","id":"r12","kind":"event","role":"haydee.dll+19A3C1#2","saved":"000007DC","sites":3}
{"k":"ref","id":"r40","kind":"window","role":"class:HaydeeWnd#0","saved":"012F10D4","sites":4}
```

Three choices matter more than the format:

1. **Addresses are written as module plus offset wherever possible**
   (`haydee.dll+2E1F70`, not `6B4D1F70`). A role key then still means the
   same thing when a module loads at a different base, which is the whole
   cross-launch problem.
2. **Each fact is decided once, at save time.** A thread's role key is
   computed when it's written. The stand-in count, the transplant, handle
   steering and the fault report all read that same key, so they can't
   disagree.
3. **References get ids.** Every outside object the game holds gets one `ref`
   line: event, thread, file, window, OpenAL id, Steam context. A load
   resolves all of them in one pass against the live process. That pass
   replaces the separate per-subsystem fixups we have now (events matched by
   creation site, files reopened, windows by class, OpenAL translated on
   first use).

## How it improves the engine

| Open problem | What the manifest changes |
|---|---|
| Thread pairing and stand-ins disagree | One role key per thread, written at save, used everywhere |
| Config drift (PhysX turned off between save and load) | Already partly solved by `.cfg`; it moves into the header and becomes a refusal or a warning by policy |
| Changing `Slot` breaks old saves | Versioned lines; unknown kinds are skipped; old saves still read |
| Handle, window and OpenAL fixups scattered across the code | One `ref` table and one resolver pass; a miss is named, not silent |
| Relocation scans all of restored memory | A pointer index (phase 2) visits only words known to hold pointers |
| Rebase and seed saves cost another full 2 GB | Base plus delta (phase 4): a rebase writes only changed chunks |
| A new game needs knowledge from the code | Game-specific facts (PhysX workers, Steam, OpenAL) become manifest entries and a per-game profile, not code paths |
| Crash triage | Python can answer "what was this address at save time?" from the manifest without our DLL |

What it won't do by itself is fix crashes such as the PhysX first-frame
fault. It makes them easier to explain, and it stops the engine from
contradicting itself.

## Phases

Each phase ships on its own and leaves the old path working.

1. **Write the manifest, don't use it yet.** Write `.json` next to `.meta` on
   every save: header, config, modules, regions, threads with role keys. Add a
   check that compares the manifest with what the load computes on its own and
   logs any disagreement. Low risk, and it immediately shows where our code
   disagrees with itself.
2. **Make the load read it.** Thread roles for the stand-in count and the
   transplant come from the manifest. Config comparison moves into it, and
   `.cfg` goes.
3. **Pointer index.** At save time, or on a background thread right after
   it, record the file offsets of every word that points into memory that can
   move (stacks, our arenas, re-created heaps), in a binary `.ptrs` file.
   Relocation then visits only those words. On Haydee that should be a few
   hundred thousand words, not around 500 million.
4. **Reference table.** Move one subsystem at a time onto `ref` lines and the
   single resolver: events first (most often wrong), then windows, files,
   OpenAL and Steam.
5. **Base plus delta.** A save names a base slot and stores only the chunks
   whose hashes changed (the hashes are already computed). That gives cheap
   rebase saves and keeps the original save point exact.
6. **Retire `.meta`.** Once the load reads everything from the manifest and
   the binary files, the raw struct copy goes, and with it the layout
   fragility.

## Phase inventories

A cross-launch load has to know what in the process existed before the game
ran, because none of it is the save's to restore. The phase inventories list
it, one file per launch phase. The same JSON Lines format is used, under
`d3d9sw_phases\<pid>_<ordinal>_<phase>.jsonl` beside the DLL. They are off
unless `D3D9SW_PHASES=1` is set in the environment or in `d3d9_sw.cfg`.

| Phase | Taken at | Holds |
|---|---|---|
| `pregame` | first line of our `DllMain` | loader, OS and WoW64 state before game code |
| `dll-load` | end of our `DllMain` | what our hooks added *(planned)* |
| `game-start` | first call from the game | CRT, window *(planned)* |
| `game-to-dll` | `Direct3DCreate9` / `CreateDevice` | our device, rebuilt on load *(planned)* |
| `game` | first save | the start of what a seed carries *(planned)* |

Each file contains these record kinds:

- `module` and `module64` for the 32-bit and 64-bit loader lists.
- `thread` with its thread block (TEB), stack, 64-bit TEB, 64-bit stack, and start address as `module+offset`.
- `heap` for 32- and 64-bit heaps, including segments and large blocks.
- `handle` with type, and name where asking cannot block. A marker event checks that this process's handles are read correctly.
- `alloc` for every allocation in the address space, with its owner.
- A closing `check` line.

Each allocation's owner comes from the first of these rules that applies, strongest first:

1. **A named span.** These are modules, stacks, TEBs, heaps, and the regions the 32-bit and 64-bit process blocks (PEBs) point at (API-set map, CSR, NLS, shim, activation contexts and so on). It also covers named mapped files.
   The 32-bit heaps come from `GetProcessHeaps()`, because the PEB's own list stops after the process heap on current Windows. Each extra heap is named after the module variable that holds its base, for example `heap:msvcrt.dll+BDB68`.
2. **`child:<owner>:<type>:<size>`.** This applies to private memory only. At least two aligned words in the header (its first 64 bytes) point into one owner, which is what a list link back looks like. That owner can be a held block. Heaps' LFH block zones land here. This rule goes before `held`, because a variable at a block's base can be a cursor that stopped there in one launch.
3. **`held:<module+offset>`.** A writable module variable, 32- or 64-bit, points at the allocation's *base*. Every such variable is listed in `base_refs`, and the diff pairs on any of them, as long as type and size also match. A variable pointing further in is a cursor and moves between launches, so it does not count.
4. **`unknown:<type>:<size>:<protect>`.** Nothing above applies. The first words are printed, and the shape is the pairing key.

`tools\phasediff.ps1 A.jsonl B.jsonl [-Detail]` pairs two launches by identity
and never by address:

- modules by name;
- threads by start address;
- heaps by owner;
- handles by type and name, with the process id normalised;
- allocations by owner, or by any shared `base_refs` entry.

For each category it reports what paired, what kept its address, what moved,
and what has no partner. Two launches of the stand-in pair completely. The
one `unknown` there is a 20 MB read-only unnamed section headed `CCDDCCDD`,
with NLS cursors into it. It is paired by its unique shape and not given a
name it hasn't earned.

## Constraints

- **No heap allocation while frozen.** The manifest is written from a fixed
  buffer in our own memory, and read before the freeze, as `.cfg` is now.
- **No added save cost.** Phase 1 is a few hundred KB of text. The pointer
  index (phase 3) is the one real cost and can run after the game resumes.
- **Diagnostics stay opt-in.** Following today's change, verification and the
  inventory are off by default; the manifest doesn't change that.
- **Game-agnostic.** Rabi-Ribi, Haydee and DDPR use the same format. Game
  facts live in entries, not in format changes.
