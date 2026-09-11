# Wholesale DOL: RBO_Ex3 on GameCube

The lean/FULL FOB VM stays. This is the other axis: take `RBO_Ex3.exe` as
the program, put a Win32 / DirectX 7 host under it, and link a second DOL.
It will not play for a long time. That is expected. The proto DOL
(`rbo_fabre_proto` / `RBO.DOL`) is unchanged.

Build: `make wholesale` → `sd_pack/RBO/RBO_EX3.DOL`

## Why the FOB path hit zeros

Stage 1 FULL only interprets `STAGE01.FOB`. Retail still needs the rest of
the EXE: char `.dat`/`.fob` loaders, DirectDraw surfaces, DirectSound
threads, IMG/PAC, and the mode FSM at `DAT_0256bd30`. Filling FOB opcodes
without that host leaves `HOST_D` with nothing to draw even after binds.

## What "the entire binary" means here

`RBO_Ex3.exe` is **x86 PE**. GameCube is **PowerPC**. The EXE cannot run.
Wholesale means:

1. **Recompile** Ghidra-lifted C (`EXTRACTION/re/RBO_Ex3/09_functions_c`,
   3309 files, 3257 `FUN_*`, 2674 `DAT_*`) for PPC.
2. **Implement every import** the PE actually has (12 DLLs, ~150 symbols)
   as a host on libogc. If the game calls DirectDraw, the host *is*
   DirectDraw (COM vtables, `Blt` / `Flip` / `Lock`), backed by GX.
3. **Bring initialized `.data` / `.rdata`** from the PE. Reconstruct BSS
   from named `DAT_*` symbols, not a 1:1 35 MB image (see MEM1).
4. **Walk the boot backwards.** CRT → `FUN_00440690` → `CoInitialize` →
   config → `DirectDrawCreateEx` → DirectInput → DirectSound → PAC/IMG →
   window loop. Each unimplemented call is a named trap on screen. Fill
   that call. Repeat until the DOL stays in the retail main loop.

Not in scope: x86 emulation, wine, or linking Microsoft `DDRAW.DLL`.

## PE facts (English `RBO_Ex3.exe`)

| Field | Value |
|-------|-------|
| File | 671948 bytes |
| ImageBase | `0x00400000` |
| SizeOfImage | `0x02172000` (35069952) |
| `.text` | VA `1000`, 544325 bytes |
| `.rdata` | VA `86000`, 12020 bytes |
| `.data` | VA `89000`, **VSz 34498168**, raw **106496** |
| `.rsrc` | VA `2170000`, 4156 bytes |
| Subsystem | WINDOWS_GUI |

Almost all of `.data` is BSS (zero-fill). Windows demand-pages 33 MB.
GameCube MEM1 is **24 MB total** (code + heap + GX FIFO + this BSS).
A 1:1 PE map will not load. Named `DAT_*` packing is mandatory.

Entry into game code after CRT: **`FUN_00440690`**
(`CoInitialize` → config → DD/DI/DS → PAC → `while` message/game loop).

Window / title: `FUN_0047a000`. DD init: `FUN_00415b70` / `FUN_00416cf0` /
`FUN_00417bd0`. DS: `FUN_00419b30`. DI: `FUN_00418850`.

## Import surface (implement all of it)

Source: `EXTRACTION/re/RBO_Ex3/01_imports_detailed.txt`

| DLL | Role on GC |
|-----|------------|
| **DDRAW** | `DirectDrawCreateEx` + full `IDirectDraw7` / `IDirectDrawSurface7` / clipper / palette vtables. GX + CPU surfaces. |
| **DSOUND** | `DirectSoundCreate8` + `IDirectSound8` / `IDirectSoundBuffer8`. ASND/AESND. |
| **DINPUT8** | `DirectInput8Create` + device `GetDeviceState`. PAD / keyboard map. |
| **WINMM** | `timeGetTime` / `timeBeginPeriod`. `gettime` / VI. |
| **OLE32** | `CoInitialize` / `CoCreateInstance` (DirectShow graph later). |
| **GDI32** | `BitBlt` / `TextOutA` / palette. Software DC on a DD surface. |
| **USER32** | HWND fake, `PeekMessageA` / `DispatchMessageA`, `MessageBoxA` → on-screen log, timers. |
| **KERNEL32** | File, heap, threads, CS, time, env. Files → libfat `sdc:` / `sda:` / DVD. Heap → `memalign(32)`. Threads → LWP. |
| **ADVAPI32** | Registry. Return "not found"; config from SD `RBO/LocalConfig`. |
| **SHELL32** | Browse-for-folder. Stub / skip installer UI. |
| **SHLWAPI** | `PathCombineA` / `PathFileExistsA` / `PathRenameExtensionA`. Real string ops. |
| **IMM32** | IME. No-op success. |

DirectShow (`opening.mpg`) is `CoCreateInstance` + FilterGraph, not a
separate import DLL. Host it when OLE32 `CoCreateInstance` is hit.

## COM / calling convention

Ghidra already emits COM as:

```c
(**(code **)(*obj + offset))(obj, ...);
```

`this` is argument 0. PPC uses that C ABI. Host vtables are arrays of C
functions; `lpVtbl` is the first pointer in each object. Offsets we have
already matched on `IDirectDraw7`:

| Off | Method |
|-----|--------|
| `+0x08` | `Release` |
| `+0x20` | `EnumDisplayModes` |
| `+0x50` | `SetCooperativeLevel` |
| `+0x54` | `SetDisplayMode` |
| `+0x6c` | `GetDeviceIdentifier` |

`IDirectSound8 +0x18` is `SetCooperativeLevel`.

Fill every vtable slot. Unimplemented slots call `host_trap(name)` and
return `E_NOTIMPL` / `DDERR_UNSUPPORTED`. Do not leave NULLs.

## Memory plan

```
MEM1 24 MB
  DOL .text/.rodata/.data   (host + lifted C; grow as functions ingest)
  packed DAT_* BSS          (named globals only, not 33 MB PE hole)
  GX FIFO 256 KB
  DD surfaces               (640x480 RGB565 backbuffer ~614 KB; more as CreateSurface)
  heap                      (HeapAlloc / VirtualAlloc / GlobalAlloc)
ARAM 16 MB
  streaming BGM / large WAV / overflow surfaces (DMA in)
SD / GCM
  DATA/*.PAC, Movie, SaveData, LocalConfig  (CreateFileA remap)
```

Rules:

- Never reserve the PE's 33 MB hole.
- `tools/collect_dat_symbols.py` lists every `DAT_*` from Ghidra C.
- `tools/pack_pe_data.py` emits initialized `.data` / `.rdata` as a
  `.o` plus a BSS of packed named symbols (sizes from Ghidra or next-VA).
- 32-byte align anything GX or ARAM will touch; `DCFlushRange`.
- If a lifted function indexes a giant static buffer, replace that `DAT_*`
  with an SD/ARAM-backed pointer rather than growing BSS.

## Ingest pipeline (game code)

Do not compile all 3309 files in one shot. They are Ghidra C (`undefined4`,
MSVC CRT, SEH). Pipeline:

1. **Host DOL** (this pass) — GC boot + complete import vtables + Flip loop.
   No lifted `FUN_*` yet. Proves DirectDraw/DirectSound/DirectInput exist.
2. **Types header** — `host/ghidra_types.h` (`undefined4`, `byte`, `code`,
   `HRESULT`, …) so a lifted file can compile.
3. **One function at a time from `FUN_00440690`.** Linker errors name the
   next `FUN_*` / `DAT_*`. Add that file (cleaned) or a stub that
   `host_trap`s. Same direction as "build backwards".
4. **CRT skip.** Do not bring MSVC startup / `RtlUnwind` / mbcs. `main`
   on GC calls `FUN_00440690` directly after `host_init`.
5. **SEH / `UnhandledExceptionFilter`.** Map to a panic screen.
6. **Keep proto.** `source/main.c` scene chain stays the playable DOL.

Optional later: a script that wraps each `09_functions_c/*.c` with the
types header and attempts `powerpc-eabi-gcc -c`, then a report of the
first N errors.

## File remap

PC paths in the EXE look like `.\data\...`, `Data\Movie\opening.mpg`,
`__LocalConfig__`. Host `CreateFileA`:

| PC | GC |
|----|----|
| `Data\...` / `.\data\...` | `sdc:/RBO/DATA/...` then `sda:` then DVD FST |
| `__LocalConfig__` | `sdc:/RBO/LocalConfig` |
| `*.sv4` / `KeyConfig.v4` | `sdc:/RBO/SAVE/` |

Log every path. Missing file is a trap with the path on screen, not a
silent `INVALID_HANDLE_VALUE`, until the boot path is known.

Title stills on the card are baked 640x480 IMGs in
`sd_pack/RBO/DATA/CG/Title/Title_Bg/`. The 1024x512 sources are **not**
in `RBO/`; they live at `F:\rbo_fabre_proto\title_bg_src1024\`. Rebake
with `python tools/bake_title_bg_640.py`.

## Success ladder (failure is data)

Each rung is a DOL that boots and shows the last host call.

1. **Host alive** — GX clear, log, START → Swiss. DirectDraw Flip of a
   filled backbuffer. DirectSoundCreate8 + DirectInput8Create succeed.
2. **`FUN_00440690` linked** — reaches `CoInitialize`, then the next trap.
3. **DirectDraw init from lifted C** — `FUN_00415b70` / `EnumDisplayModes`
   callback `LAB_00417b70`.
4. **Window / PeekMessage loop** — fake HWND, `PeekMessageA` pumps PAD.
5. **CreateFile of first PAC** — path visible; implement `rbo_pac` XOR
   `0xE3DF59AC` on that handle.
6. **IMG / title plate via DD surfaces** instead of our TPL scene FSM.
7. **Mode 5 GameMain** — FOB VM can attach here instead of STSEL FULL.
8. **Char dat+fob** — `FUN_00444c80`. Then Stage 1 enemies are the EXE's
   problem, not a second spawn table.

The playable proto remains the way to actually fight on hardware until
rung 7–8.

Hardware today is a **mode-0 stand-in title** (rung 6, shortcut). Retail
cold boot is mode `0xB`, not mode 0. Remaining functions and modes are a
checkoff list in [WHOLESALE_CHECKLIST.md](WHOLESALE_CHECKLIST.md).

Play path (title → lobby mode 4 → stsel mode 6 → stage 1 on mode 0):
[WHOLESALE_PLAY_PATH.md](WHOLESALE_PLAY_PATH.md). Lobby is GameMain
`FUN_004090f0` / Lobby.Img, not mode 3 Ranking.Img.

## Layout in this repo

```
host/include/     Win32 + DX7 headers (PPC, this-as-arg0)
host/*.c          Import implementations
wholesale/main.c  GC crt0-style boot → host_winmain
wholesale/Makefile
tools/collect_dat_symbols.py
docs/WHOLESALE_DOL.md        (this file)
docs/WHOLESALE_CHECKLIST.md  remaining FUN_* / modes / host traps
docs/WHOLESALE_PLAY_PATH.md  title → lobby → stsel → stage 1
```

Proto map in [PORT_SKELETON.md](PORT_SKELETON.md) still describes the
*thin* modules. This file is the *thick* host.

## Hardware

Primary test is still a real Cube (Swiss, SD2SP2). Dolphin is for the
Flip path and log. MEM% / panic text must be readable without a USB
Gecko. START/RESET still go through `escape.h`.
