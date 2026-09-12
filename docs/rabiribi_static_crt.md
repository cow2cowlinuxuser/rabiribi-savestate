# Rabi-Ribi static CRT: discovery, failure mode, and isolation plan

## Discovery (2026-09)

Rabi-Ribi **statically links the C runtime**. Its import table names only
system DLLs:

    KERNEL32.dll
    USER32.dll
    GDI32.dll
    SHELL32.dll
    STEAM_API.dll

No CRT DLL appears — no `ucrtbase.dll`, no `MSVCR*.dll`, no `api-ms-win-crt-*`.
The allocator entry points live in the game's own `.text` section.

Symbols in the image confirm a statically linked UCRT (`__acrt_locale_free_monetary`
and similar internal names).

## Complete Heap\* call-site surface

Source: `ghidra_surface.txt` from the RRAllocSurface Ghidra script, walking
every call site of `HeapAlloc` / `HeapFree` / `HeapReAlloc` / `HeapSize` in
`rabiribi.exe.dump_00850000.exe` (image base `0x00850000`).

`HeapCreate` / `HeapDestroy`: **none** — the game never creates a private heap.

### What counts as an allocator

A function that contains a `HeapAlloc` call is not an allocator. The test is
whether it reads the runtime's cached heap handle at `0x0118E588` — that global
is `__acrt_heap`, and everything that shares it is one allocator with one set of
blocks. A function that calls `GetProcessHeap()` for itself is ordinary code
that happens to allocate, and it cannot be detoured as an allocator because it
has no pointer parameter to dispatch on.

This distinction decides the whole isolation plan, and an earlier revision of
this document got it wrong in both directions. It is applied strictly below.

### CRT allocator set

These are the statically linked UCRT functions. All five read `__acrt_heap` at
`0x0118E588`, which the static UCRT sets to `GetProcessHeap()`.

The image contains **no call to `HeapCreate` anywhere**, which settles that
independently: a static runtime that made its own heap would have to call it.
That is stronger evidence than the convention, and it is the reason to stop
arguing about this one.

| function        | RVA        | Heap API         | prologue (first 16 bytes)                                     |
|-----------------|------------|------------------|---------------------------------------------------------------|
| `_malloc`       | `0x36E6B2` | HeapAlloc        | `55 8B EC 56 8B 75 08 83 FE E0 77 6F 53 57 A1 88`            |
| `_free`         | `0x369754` | HeapFree         | `55 8B EC 83 7D 08 00 74 2D FF 75 08 6A 00 FF 35`            |
| `_realloc`      | `0x36E744` | HeapReAlloc      | `55 8B EC 83 7D 08 00 75 0B FF 75 0C E8 5D FF FF`            |
| `__calloc_impl` | `0x37EBF7` | HeapAlloc        | `55 8B EC 56 8B 75 08 85 F6 74 1B 6A E0 33 D2 58`            |
| `_msize`        | `0x37805A` | HeapSize         | `55 8B EC 83 7D 08 00 75 15 E8 B6 65 FF FF C7 00`            |

`_realloc`'s null-pointer path calls `_malloc` at `0x36E6B2` (confirmed).

`_expand`, `_recalloc`: **not present** in this surface. The set is five.

Three other functions in the image are `free`-shaped, and none of them touches
`HeapFree` — they tail-call `_free` at `0x369754`. The set is therefore
**closed**, which is what makes detouring it safe: a block allocated through a
hook and released through a routine outside the set would go to `HeapFree`
against a heap that never issued it.

### Not allocators — ordinary code that calls Heap APIs

These call `GetProcessHeap()` for themselves and never read `__acrt_heap`. None
of them is a detour target, and an earlier revision of this document was wrong
to list `FUN_00bdbe12` in the CRT set above.

| function        | RVA        | Heap API            | prologue (first 16 bytes)                                     | notes |
|-----------------|------------|---------------------|---------------------------------------------------------------|-------|
| `FUN_00bdbe12`  | `0x38BE12` | HeapAlloc + HeapFree | `55 8B EC 83 EC 18 53 56 57 8B 7D 08 33 F6 6A 01`           | allocates, works, frees, all inside one call — a self-contained pair with no exchange surface |
| `FUN_0087bc20`  | `0x2BC20`  | HeapAlloc           | `55 8B EC 81 EC C8 04 00 00 A1 40 E9 D1 00 33 C5`            | 0x4c8 stack frame and a security cookie; builds something, not a general allocator |
| `FUN_0087c070`  | `0x2C070`  | HeapFree            | `A1 60 EA D4 00 56 48 74 0C 48 75 0E E8 EF A1 07`            | **takes no pointer argument** — reads a global mode flag, dispatches, frees what it computed. Not `free(void *)` |
| `FUN_008dfef0`  | `0x8FEF0`  | HeapFree            | `E8 3B 08 00 00 E8 86 FD FF FF 83 3D 58 EA D4 00`            | teardown/shutdown |

**These cannot be detoured, and not only because of what they are.**
`FUN_008dfef0` begins with `E8 3B 08 00 00` — a relative `CALL` as its first
instruction. Copy those five bytes to a trampoline and the rel32 resolves
somewhere else entirely. Verifying prologue bytes and *relocating* them are
different questions, and a 16-byte comparison answers neither: what a detour
needs is that the bytes it displaces contain no relative operand and end on an
instruction boundary. The five CRT functions each have a clean 7-byte prologue
by that test. These do not.

### IAT data slots

HeapAlloc / HeapFree / HeapReAlloc / HeapSize appear in the IAT as data
thunks (the import address table entries the loader resolves). These are the
call targets the CRT and the non-CRT paths reach through — not separate
functions to detour, but the mechanism by which the functions above reach
`ntdll!RtlAllocateHeap` et al.

## What was wrong

`game_crt_heap()` iterated a list of known CRT DLL names, found `ucrtbase.dll`
loaded in the process, called `_get_heap_handle()` on it, and logged:

    game runtime heap 00xxxxxx from ucrtbase.dll, which IS the process heap

The log was correct that the game's heap is the process heap. It was wrong about
*why*: the game does not call ucrtbase at all. The `ucrtbase.dll` present in the
process belongs to **Windows** — every modern Windows process loads it for its
own use. The static UCRT compiled into the game sets its heap to
`GetProcessHeap()`, which is why the observation (shared process heap) was right
even though the attribution (from ucrtbase as the game's CRT) was wrong.

## Why this matters for isolation

The engine's allocator hooks work by patching **import tables** (IAT). An IAT
redirect catches every call a module makes through its import thunk. For a game
that dynamically links ucrtbase, patching the game's import of `malloc` redirects
every `malloc` call the game makes.

For a game that statically links the CRT, there is no import thunk to patch.
The game's `malloc` is a private function at `exe_base + 0x36E6B2`. IAT
redirects into ucrtbase.dll are useless: they redirect Windows' copy of the
allocator, which the game never calls.

The correct isolation path is to **detour the game's private CRT copies** at
`exe_base + RVA`. This is a standard inline hook: overwrite the first bytes of
the function with a jump to a replacement, and call the original through a
trampoline that preserves the overwritten prologue.

## The cross-path question, and why it no longer gates anything

An earlier revision held that a CRT-only private heap was incomplete isolation,
because `FUN_0087bc20` / `FUN_0087c070` also touch the process heap, and that
arming had to wait until the two sets were proven never to exchange blocks.

Two things were wrong with that.

**Co-tenancy is not exchange.** Two pieces of code sharing `GetProcessHeap()`
can run forever without trading a single block. Sharing an arena is not sharing
objects, and the earlier text treated the first as evidence of the second.

**The risk is asymmetric, and one side was already handled.** A block from
`FUN_0087bc20` arriving at our `free` is fine: the ownership check fails and the
pointer is forwarded to the original `free`, which returns it to the heap it came
from. Only the reverse direction — one of our blocks reaching a `HeapFree` we do
not control — could corrupt anything.

And that direction does not need proving, because it can be made impossible.
Every `HeapFree` in the executable, the CRT's and the other three alike, goes
through **one import slot**. Patching it puts an ownership check underneath every
freer in the process, including any that were never identified:

```
HeapFree IAT slot  ← one patch
  ├── _free                (the CRT set)
  ├── FUN_0087c070
  ├── FUN_008dfef0
  └── FUN_00bdbe12
```

The census reports how many of our blocks arrived by a non-CRT route. **Zero over
a long session is the proof the earlier revision asked for**; anything above zero
is a wrong-heap free that was caught instead of shipped. Measuring and fixing are
the same change, which is why the experiment was not worth running separately.

## Implemented isolation (armed behind `D3D9SW_GAMEHEAP=1`)

Implemented in `gameheap.c`, not in `savestate.c`.

1. `HeapCreate` a private heap.
2. Verify all five prologues. **One mismatch aborts the whole install** — an RVA
   is meaningless against a build it was not read from, and patching four of five
   is worse than patching none.
3. Build every trampoline *before* writing any jump, so a call arriving midway
   through patching reaches a handler that can already forward.
4. Detour `_malloc`, `__calloc_impl`, `_realloc`, `_free`, `_msize`.
5. Patch the `HeapFree` import slot as the floor described above.

Allocations at or above 256 KB stay with the runtime: a private heap serves those
from a dedicated `VirtualAlloc` released on free, which is the one way a range in
our ownership table goes stale. They are also rare and long-lived — which is to
say they are not the state that fails to travel.

Ownership is not decided by address alone. A range table records where our heap
has been seen, and every block carries an 8-byte header whose magic is keyed to
the pointer it precedes. The range check runs first because it cannot fault; the
header is only read once the range says the memory was ours, and it is what
actually decides.

**Rejected:** writing our handle into `__acrt_heap` and patching no code at all.
Everything already allocated came from the process heap, and the next `free`
would offer it to a heap that never issued it. That swap is only safe before the
runtime initialises, and the wrapper arrives by `LoadLibrary` long after. Detours
dispatch per pointer, so the mixed period stays correct by construction.

## Prologue verification

Before overwriting any function, the bytes at `exe_base + RVA` are compared
against the expected prologue from the Ghidra dump above, and the page is
confirmed committed. This is the same pattern `allocwatch.c` uses for the
`rep movsd` at `+0x9B730`: confirm the instruction before touching it.

The check is 7 bytes, not 16, because 7 is what the detour displaces — and the
relevant property is not only that the bytes match but that they can be
*relocated*: no relative operand, ending on an instruction boundary. All five CRT
prologues satisfy that. `FUN_008dfef0` does not, as noted above.

## Files changed

- `gameheap.c` — private heap, header-tagged allocations, range table, the five
  detours with trampolines and prologue verification, and the `HeapFree` floor.
- `savestate.c` — `exe_crt_import()` added; `game_crt_heap()` distinguishes
  static-CRT from dynamic-CRT and logs the truth; `savestate_game_heap()` so the
  private heap is classified as the game's runtime.
- `tools/ghidra/RRAllocSites.java`, `tools/ghidra/RRAllocSurface.java` — the
  scripts that produced this, with their output alongside.
- `pe_imports.ps1` — cannot answer this one from disk: `rabiribi.exe` carries a
  Steam DRM `.bind` section and its import table is not readable statically. The
  question was settled from the live image instead.
