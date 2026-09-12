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

### CRT allocator set

These are the statically linked UCRT functions. All use the process heap via
the CRT's cached heap handle (which the static UCRT sets to `GetProcessHeap()`).

| function        | RVA        | Heap API         | prologue (first 16 bytes)                                     |
|-----------------|------------|------------------|---------------------------------------------------------------|
| `_malloc`       | `0x36E6B2` | HeapAlloc        | `55 8B EC 56 8B 75 08 83 FE E0 77 6F 53 57 A1 88`            |
| `_free`         | `0x369754` | HeapFree         | `55 8B EC 83 7D 08 00 74 2D FF 75 08 6A 00 FF 35`            |
| `_realloc`      | `0x36E744` | HeapReAlloc      | `55 8B EC 83 7D 08 00 75 0B FF 75 0C E8 5D FF FF`            |
| `__calloc_impl` | `0x37EBF7` | HeapAlloc        | `55 8B EC 56 8B 75 08 85 F6 74 1B 6A E0 33 D2 58`            |
| `_msize`        | `0x37805A` | HeapSize         | `55 8B EC 83 7D 08 00 75 15 E8 B6 65 FF FF C7 00`            |
| `FUN_00bdbe12`  | `0x38BE12` | HeapAlloc+Free   | `55 8B EC 83 EC 18 53 56 57 8B 7D 08 33 F6 6A 01`            |

`_realloc`'s null-pointer path calls `_malloc` at `0x36E6B2` (confirmed).

`_expand`: **not present** in this surface.

### Non-CRT allocator paths

These functions call `GetProcessHeap()` + `HeapAlloc` / `HeapFree` directly,
bypassing the CRT entirely. They are game-specific or engine-specific code.

| function        | RVA        | Heap API            | prologue (first 16 bytes)                                     | notes |
|-----------------|------------|---------------------|---------------------------------------------------------------|-------|
| `FUN_0087bc20`  | `0x2BC20`  | HeapAlloc+GetProcHeap | `55 8B EC 81 EC C8 04 00 00 A1 40 E9 D1 00 33 C5`          | game/custom allocator |
| `FUN_0087c070`  | `0x2C070`  | HeapFree            | `A1 60 EA D4 00 56 48 74 0C 48 75 0E E8 EF A1 07`            | game/custom free |
| `FUN_008dfef0`  | `0x8FEF0`  | HeapFree            | `E8 3B 08 00 00 E8 86 FD FF FF 83 3D 58 EA D4 00`            | teardown/shutdown |

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

## The isolation problem: CRT-only is not enough

The surface analysis revealed that moving **only the CRT set** to a private
heap does not fully isolate the process heap. Two non-CRT functions
(`FUN_0087bc20` and `FUN_0087c070`) also call `GetProcessHeap()` +
`HeapAlloc` / `HeapFree` directly — outside the CRT. If:

1. The CRT `_malloc` is redirected to a private heap, but
2. `FUN_0087bc20` still allocates from the process heap, and
3. A block allocated by `FUN_0087bc20` is later freed by `_free` (now on the
   private heap), or vice versa —

then `HeapFree` is called on the wrong heap: silent corruption or crash.

Before enabling private-heap detours, the design must either:

- **Prove isolation**: demonstrate that the CRT set and the non-CRT set never
  exchange blocks (i.e. nothing allocated by `FUN_0087bc20` is ever freed by
  `_free`, and nothing `_malloc`'d is ever freed by `FUN_0087c070`).
- **Detour both sets**: redirect the non-CRT paths to the same private heap.

`FUN_008dfef0` is a teardown/shutdown path and is likely safe to leave on the
process heap, but this needs confirmation.

## Planned private-heap isolation (not yet enabled)

Once the cross-path question is resolved:

1. Create a private heap with `HeapCreate`.
2. Detour `_malloc` → allocate from the private heap.
3. Detour `_free` → free on the private heap.
4. Detour `_realloc` → reallocate on the private heap.
5. Detour `__calloc_impl` → allocate-and-zero on the private heap.
6. Detour `_msize` → query size on the private heap.
7. Detour `FUN_00bdbe12` → both alloc and free on the private heap.
8. Account for `FUN_0087bc20` / `FUN_0087c070` (either detour or prove disjoint).

The process heap then serves only Windows, and the game's allocations live in a
heap this engine fully controls. The boundary problem described in
`restore_invariants.md` — one heap serving both the game and Windows — is
eliminated rather than managed.

## Prologue verification

Before overwriting any function, the first 16 bytes at `exe_base + RVA` are
compared against the expected bytes from the Ghidra dump above. A mismatch
means the executable is a different build, and the detour is not applied. This
is the same pattern `allocwatch.c` uses for the `rep movsd` at `+0x9B730`:
confirm the instruction before touching it.

All prologue bytes are now filled from `ghidra_surface.txt`. The scaffold
verifies them at runtime and reports which match.

## Files changed

- `savestate.c` — `exe_imports_dll()`, `exe_imports_any_crt()` added;
  `game_crt_heap()` now distinguishes static-CRT from dynamic-CRT and logs
  the truth; detour scaffold with prologue bytes for all 9 functions in the
  Heap* surface (6 CRT, 3 non-CRT), runtime verification, gated off.
- `docs/rabiribi_static_crt.md` — this file.
- `pe_imports.ps1` — unchanged; it already answers the same question from the
  command line (run it on `rabiribi.exe` to see `VERDICT: imports name no C
  runtime`).
