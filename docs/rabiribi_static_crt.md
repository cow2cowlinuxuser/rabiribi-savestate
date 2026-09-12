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
The allocator entry points live in the game's own `.text` section:

| function          | RVA          |
|-------------------|--------------|
| `_malloc`         | `0x36E6B2`   |
| `_free` / `__free_base` | `0x369754` |
| `_realloc`        | `0x36E744`   |
| `__calloc_impl`   | `0x37EBF7`   |

Symbols in the image confirm a statically linked UCRT (`__acrt_locale_free_monetary`
and similar internal names).

**Still under investigation:** whether `_msize` and `_expand` exist in the image.
Both are called by the static CRT's `realloc` internally; if they exist at
separate RVAs they need detouring too.

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

## The free-set closure problem

Hooking `malloc` without hooking **every** free-like function is worse than
hooking nothing. If `malloc` is redirected to a private heap but `free` still
calls the original (which calls `HeapFree` on the process heap), then:

1. `malloc` returns a block from the private heap.
2. `free` calls `HeapFree(GetProcessHeap(), block)`.
3. The process heap does not own that block → silent corruption or crash.

Three free-like sites have been identified in one session (by reference count):

| address      | references | likely function  |
|-------------|-----------|-----------------|
| `00bb9754`  | 365       | `_free` / `__free_base` |
| `00bba41a`  | 7         | (variant)       |
| `00bba441`  | 87        | (variant)       |

The full set is being enumerated via a Ghidra script (`RRAllocSurface`) that
walks call sites of `HeapAlloc` / `HeapFree` / `HeapReAlloc` / `HeapSize`.

**Until the free-set closure is confirmed, no live hooking is safe.** The
detour scaffold exists in `savestate.c` (gated off by the environment variable
`D3D9SW_RR_DETOUR=1`) with prologue verification stubs, but it must not be
enabled until every function that can pass a game-allocated block to `HeapFree`
has a matching detour.

## Planned private-heap isolation (not yet enabled)

Once the allocator surface is closed:

1. Create a private heap with `HeapCreate`.
2. Detour `_malloc` → allocate from the private heap.
3. Detour `_free` (and every variant) → free on the private heap.
4. Detour `_realloc` → reallocate on the private heap.
5. Detour `__calloc_impl` → allocate-and-zero on the private heap.
6. Detour `_msize` / `_expand` if present.

The process heap then serves only Windows, and the game's allocations live in a
heap this engine fully controls. The boundary problem described in
`restore_invariants.md` — one heap serving both the game and Windows — is
eliminated rather than managed.

## Prologue verification

Before overwriting any function, the first ~16 bytes at `exe_base + RVA` are
compared against expected bytes from a Ghidra dump. A mismatch means the
executable is a different build, and the detour is not applied. This is the
same pattern `allocwatch.c` uses for the `rep movsd` at `+0x9B730`: confirm the
instruction before touching it.

Expected bytes are placeholder (`TODO`) until the author pastes the Ghidra dump
for each function. Without them the prologue check refuses to arm.

## Files changed

- `savestate.c` — `exe_imports_dll()`, `exe_imports_any_crt()` added;
  `game_crt_heap()` now distinguishes static-CRT from dynamic-CRT and logs
  the truth; detour scaffold for rabiribi.exe allocator RVAs (gated off).
- `docs/rabiribi_static_crt.md` — this file.
- `pe_imports.ps1` — unchanged; it already answers the same question from the
  command line (run it on `rabiribi.exe` to see `VERDICT: imports name no C
  runtime`).
