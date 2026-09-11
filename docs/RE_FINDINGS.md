# RBO Ex3 RE Findings (Ghidra bulk export)

Source: headless export of `ENGLISH PATCH\ENGLISH PATCH\RBO_Ex3.exe`  
Artifacts: `EXTRACTION\re\RBO_Ex3\` (`07_disasm_all.txt`, imports, strings, per-fn ASM)  
Image base: `00400000` · ~3460 functions · PE x86 Windows

This is the working brief for furnishing GC `rbo_*` modules. Networking is absent on PC; omit on GC.

---

## Libraries (actual surface)

`DDRAW` `DINPUT8` `DSOUND` `GDI32` `WINMM` `OLE32` `USER32` `KERNEL32` `ADVAPI32` `SHELL32` `SHLWAPI` `IMM32`

| Layer | PC usage | GC drop-in |
|-------|----------|------------|
| Primary GFX | **DirectDraw 7** (`DirectDrawCreateEx`) | `rbo_gx` |
| GDI | `BitBlt` / `TextOutA` secondary / debug | ignore for port |
| Video | **DirectShow** FilterGraph → `Data\Movie\opening.mpg` | `rbo_movie` |
| Input | **DirectInput 8** + KeyConfig | `rbo_input` |
| Audio | **DirectSound 8** + wav / SE.pac | `rbo_audio` (ASND + SD WAV v1) |

Config strings of interest: `FULLSCREEN`, `BootScreenMode`, `BootBitDepth`, `JumpCharaSel`, `AnimeFrameSpeed`, `DrawFrameSpeed`. No rich graphics menu — GC can stay fixed 640×480 (optional later 16:9 VI flag).

---

## High-value functions

| Addr | Role | Port note |
|------|------|-----------|
| `FUN_0047a000` | Window create, title string, `ShowCursor` | Host / no HWND on GC |
| `FUN_00440690` | Bootstrap: CoInit → config → DD/DI/DS → PAC → game | `rbo_platform_init` + scene start |
| `FUN_00415b70` | DirectDrawCreateEx + DX7 gate | `rbo_gx` |
| `FUN_00418850` | DirectInput8Create | `rbo_input` |
| `FUN_00419b30` | DirectSoundCreate8 | `rbo_audio` (ASND + SD WAV) |
| `FUN_00453f70` | DirectShow graph | `rbo_movie` |
| `FUN_0043e920` | Push `opening.mpg` | Intro scene |
| `FUN_00425c90` | PAC open; header **XOR `0xE3DF59AC`**; TOC name XOR `(i*j*3+0x3D)`, size XOR key, offset @ +0x3C plain | offline sound extract; `rbo_pac` later |
| `FUN_00476ed0` | IMG loader | PacNyx→TPL today; `rbo_img` later |
| `FUN_00404960` | Caution / Boot_logo loads | Boot chain assets |
| `FUN_004777e0` | Title.Img | Title |
| `FUN_00406c70` | Lobby.Img | Lobby |
| `FUN_00467f90` | st_select.img | Stage select |
| `FUN_00444c80` | `%s.dat` + `%s.fob` char pair | `rbo_chara` |
| `FUN_00478390` | Global mode switch on `DAT_0256bd30` | `rbo_scene` |
| `FUN_004775f0` | Title menu choice switch | Title → hub/battle |
| `FUN_004042d0` / `00404400` | Boot timed FSM | Caution/logo timers |

---

## Mode FSM (PC)

**Global:** `DAT_0256bd30` dispatched by `FUN_00478390`

| Value | Likely | GC fine scenes |
|------:|--------|----------------|
| 0 | Title / boot-into-title | CAUTION, LOGO, TITLE |
| 1 | Opening / transition | INTRO |
| 3 | Lobby / CharSel / related (`DAT_0256bca8` sub) | LOAD, LOBBY, STSEL |
| 4 | Result / ranking | (stub later) |
| 5 | In-match GameMain | PLAY |

**Title menu** `FUN_004775f0` (approx): Start → hub/stage prep; Ranking → result mode; Exit; Jump toward battle variants.

**Boot:** caution frames + Boot_logo → (movie) → title.

**Sprite note:** no `PATTERN001` strings in EXE. Pose data lives in `.dat`/`.fob` + script binders (`StageInit`, `StageMainCallback`). PacNyx tables remain authoritative for compositor.

**UI sprite banks:** EXE `.data` holds `0x28`-byte records (`SrcX/Y/W/H` floats); `FUN_00476e70` stamps sheet size + `FUN_00476dc0` builds UVs. Dump: `EXTRACTION/dumps/ui_sprites/` via `tools/export-ui-sprite-banks.ps1` (optional Ghidra: `tools/ghidra_dump_ui_sprites.py`).

---

## Asset I/O

- Funnel: `CreateFileA` / `ReadFile` wrappers ≈ `FUN_00426770` / `004267d0`
- PAC packs: SE, BGM, BG01/02, DATA01/02, ETC, CG, Ex1/Ex2/Ex3 discs
- Sound v1 (GC): offline extract from `BGM.PAC` / `SE.PAC` → `sdc:/RBO/SOUND/{bgm,se}/*.wav` (mono s16le 22050) via `tools/export-sound-sd.ps1`; runtime ASND in `rbo_audio`
- UI plates under CG; scripts `Stage%02d.fob` / `Arena%02d.fob`
- **Stage 1 chain:** [STAGE1_CHAIN.md](STAGE1_CHAIN.md) — FOB exports, asset refs, GC strategy (no wholesale VM at current MEM1)
- Saves: `*.sv4`, `KeyConfig.v4`, rankings `*.rv4`
- Install path via registry — **omit on GC** (SD only)

---

## Input / present contracts (for stubs)

**Input (DI-like):**
- Poll once per frame; expose held / pressed / released
- Multi-pad: lobby pad-count / “hold A” (`Not enough pads…` strings)
- START reserved for host quit (Swiss) on GC; PC may use differently

**Present (DD Flip-like):**
- Ortho 2D → textured quads → DrawDone → EFB→XFB → WaitVSync
- Scene effects / Y-sort stay outside the Flip helper

---

## Confidence

| Solid | Medium |
|-------|--------|
| Lib set, DX7/DI8/DS8/DirectShow | Exact title-menu case→mode map |
| PAC XOR key | Full PAC compression |
| Mode switch exists at `00478390` | Every `bd30` semantic label |
| Char = dat+fob pair | On-disk pose naming |

---

## Recommended implementation order

1. Furnish `rbo_input` + `rbo_gx`; route `main.c` (this pass).
2. Model fine scenes through `rbo_scene` synced to PC modes (this pass).
3. Scene asset swaps via `rbo_assets`.
4. PAC TOC + XOR header.
5. Movie still / skip.
6. ~~Audio~~ — ASND BGM/SE from SD WAVs (v1 done; PAC-on-device later).
