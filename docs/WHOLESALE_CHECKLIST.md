# Wholesale remaining work (checkoff)

Living list for `RBO_EX3.DOL`. The playable proto (`RBO.DOL` / `source/main.c`)
is a different tree. Do not mix them.

Tick a box when that path is live on hardware, not merely ingested.

Primary success path (title → lobby → stsel → stage 1):
[WHOLESALE_PLAY_PATH.md](WHOLESALE_PLAY_PATH.md). This file stays the
full inventory. Boot plates and movie are not required for that path.

Legend:

| Mark | Meaning |
|------|---------|
| `[x]` | Live on Cube (or host is good enough that the game loop uses it) |
| `[~]` | Present but skip/stand-in/partial |
| `[ ]` | Not done. Halt stub, skip log, or never ingested |

Ingest: `python tools/ingest_wholesale.py FUN_00xxxxxx`. Handwritten files
are marked `/* wholesale-host */`. Halt stubs live in `lifted/fun_stubs.c`.

Ghidra C is ~3257 `FUN_*`. Linked wholesale is ~158 lifted files plus ~65
halt stubs (plus a handful of `LAB_*` thunks). Everything else is not in
the DOL yet. Snapshot: 2026-08-14.

---

## Where we are

Hardware is past the **mode-0 stand-in title**. A on Start sets mode 4
(lobby). A on lobby sets mode 6 (stsel). A on stsel sets mode 0 as play
(`host_set_play(1)`). Remaining Gate 3 work is FOB + STAGE01.TPL walkscape
staying in the WinMain loop on hardware.

Retail cold boot is **mode `0xB`** (`DAT_00497578 == 11` in the PE). Cube BSS
zeros that dword, and handwritten `FUN_0043f750` then **forces mode 0**.
So caution → Boot_logo → Title.Img has never run on this DOL.

`BootScreenMode FULLSCREEN` is DirectDraw only. It does not pick the scene.

---

## Skip policy (still in force)

Do not un-skip these until the callee can miss a file without setting
`DAT_00bc91a4 = 0xff` and killing the loop.

| Log | Function | Why it stays skipped |
|-----|----------|----------------------|
| TITLE skip | `FUN_0043bd00` | Stage_XX.fob |
| UNIT skip | `FUN_0046c1c0` | PE-sized zero loops |
| ENTER skip | `FUN_0043be20` | SYSTEM / result / chars; miss → mode `0xff` |
| POST skip | `FUN_0043bea0` | empty chars |
| HUD skip | `FUN_00470090` | packet pool + SYSTEM.Img HUD |
| UV skip | `FUN_00476b30` | WinMain loop helper |
| SND skip | `FUN_0043acf0` | no wav/PAC playback yet |
| DS skip | `FUN_0043ecd0` | DirectSound path |
| TIME skip | `FUN_00439290` | SystemTimeMessage.fob |
| GDATA skip | `FUN_00455770` | GameDataTable.Fob |
| CHR skip | `FUN_00456f40` | 15-file sv3 convert loop |
| DS skip / VER skip | `FUN_0043ecd0` | DirectSound + FOB GetVersion; PAC TOC still opens |
| FRAME | `FUN_0043d350` | DI stub; **does** poll pad + START |

---

## 1. Mode FSM

Retail dispatcher is `FUN_0043f750` (enter) + ingested `FUN_0043fab0` (tick).
Our enter function is a stub. Tick is the real switch, but mode 0 never
leaves, and other modes halt on the first stub they call.

| Mode | Retail enter | Retail tick / draw | Status |
|-----:|--------------|--------------------|--------|
| 0 / 1 | `3bd00` `6c1c0` `3be20` `3bea0` | `3c690` → `3c610` → stand-in `73890` | `[~]` forced; skips; fake title |
| **2** | `77770` `777e0` Title.Img | `785e0` → `77ab0` | `[ ]` halt `785e0` `78390` |
| 3 | `54cf0` `54d70` | `55730` | `[ ]` lobby / charsel |
| 4 / 5 | `09020` `090f0` | `0b490` `0f860` | `[ ]` in-match / GameMain |
| 6 | `67f60` `67f90` st_select | `68c60` | `[ ]` stage select |
| 7 | `018d0` `01b60` | `03290` | `[ ]` |
| 8 / 9 | `578d0` `57940` | `58210` | `[ ]` ranking-ish |
| 10 | `41ea0` `41f10` | `422f0` | `[ ]` |
| **0xB** | `04890` `04960` caution+logo | `04ac0` / `050e0` | `[ ]` halt `04ac0` `050e0` |
| **0xC** | `3e940` | `3e9f0` | `[ ]` `opening.mpg` |
| 0xD | `5ac80` `5ae80` | `5f060` | `[ ]` |
| 0xE | `247d0` `24820` | `24ac0` | `[ ]` |

- [ ] Stop forcing mode 0 in `FUN_0043f750`. Honor `DAT_00bc91a4`.
- [ ] Seed `DAT_00497578 = 11` (PE value) so cold boot is `0xB`.
- [ ] Enter `0xB` without the mode-0 TITLE/UNIT/ENTER bundle.
- [ ] `0xB` return 2 → mode 2 (title). Return 3 → mode `0xC` (movie).
- [ ] Mode 2 menu `FUN_004775f0` / `FUN_00478390` (Start / Ranking / Exit).
- [ ] A on Start Game must not leave title until ENTER is safe.

`DAT_0256bd30` + `FUN_00478390` is the in-title/in-hub switch, not the
coarse `DAT_00bc91a0` table above. [RE_FINDINGS.md](RE_FINDINGS.md) mixed
proto fine-scenes into PC mode 0. Use this table for wholesale.

---

## 2. Boot plates (mode `0xB`)

- [ ] `FUN_00404960` — `.\Data\CG\Title\Boot\caution%02d.Img` (01, 02) then `Boot_logo.Img`
- [ ] `FUN_00404890` / `FUN_00404850` / `FUN_00404840` enter
- [ ] `FUN_004042d0` caution timer FSM
- [ ] `FUN_00404400` three Boot_logo slices (`ui_layouts.h` `k_boot_logos`)
- [ ] `FUN_00404380` / `FUN_00404540` per-tick
- [ ] `FUN_00404ac0` (`DAT_004a2760` 0=caution, 1=logo, 2=other)
- [ ] `FUN_004050e0` draw
- [ ] Loose IMGs on `sdc:/RBO/DATA/CG/Title/Boot/` (or PAC payload)

These loaders are **not even stubbed**. They are absent from the DOL.
Ingesting `3f750` for `0xB` without them will fail at link time, then
hang on `04ac0` / `050e0` if you only stub those two.

---

## 3. Retail title (mode 2) vs stand-in

Stand-in (live, handwritten `FUN_00473890`):

- [x] Title.Img / `TITLE.TPL` id 3 atlas
- [x] Menu slices + blue bar
- [x] Title_Bg 001–008 baked 640×480, all resident, cycle by pointer
- [x] Pad edge on any port (D-pad / stick hysteresis, no auto-repeat)
- [x] Packet pool recycle (`FUN_0040fa70`); AABB blit; Flip presents RT
- [ ] A actually changes mode
- [ ] Retail pan (`DAT_0256bc80`, dest 1024×512) if we ever un-bake

Retail (not live):

- [ ] `FUN_004777e0` load Title.Img
- [ ] `FUN_00477ab0` draw Title.Img
- [ ] `FUN_004785e0` mode-2 present path
- [ ] `FUN_004775f0` menu choice
- [ ] Retire stand-in `73890` once `77ab0` draws (retail `73890` is a
      different overlay inside mode 0’s `3c610` list)

Title_Bg rebake: `python tools/bake_title_bg_640.py`  
1024 sources: `F:\rbo_fabre_proto\title_bg_src1024\` (outside `RBO/`)

---

## 4. Movie (mode `0xC`)

- [ ] `FUN_0043e920` / `FUN_0043e940` push `Data\Movie\opening.mpg`
- [ ] `FUN_00453f70` DirectShow graph
- [ ] Host `CoCreateInstance` (currently traps)
- [ ] Skip-to-title on A/B and on miss (no movie on the card must not hang)

---

## 5. PAC / IMG / files

- [x] `CreateFileA` remap `.\Data\...` → `sdc:/RBO/DATA/...`
- [x] `FUN_00476ed0` → `host_img_load_path` (fmt 0=1555, 1=raw16, 2=RGBA)
- [x] TPL hand-parse (do not `TPL_CloseTPLFile`)
- [x] `FUN_00425c90` PAC **TOC only** (XOR `0xE3DF59AC`)
- [x] `FUN_00427480` PathFileExists-style
- [ ] `FUN_00426c40` (or next ingest) **basename payload lookup**
- [ ] `CG.PAC` / `DATA01.PAC` / `DATA02.PAC` on the card, not only loose IMGs

SYSTEM.Img is already loaded on title and not drawn (HUD later).
Do not 8.3-rename retail paths.

---

## 6. Draw host (D3D / DD / packets)

- [x] IDirect3D7 software device, type-3 strips
- [x] Axis-aligned blit (title quads)
- [x] Packet pool 256, reset each tick, play type-3 only
- [x] Flip presents backbuffer; no extra 640×480 Blt
- [ ] Retail `FUN_00413d40` playback (full type switch, `__ftol`)
- [ ] Color key as a texture state, not `pix==0` on every blit
- [ ] GX path for atlases (drop CPU 640×480 RT when we can)
- [ ] Clipper / color-key / overlay DD slots if retail actually calls them

Barycentric `fill_tri` stays for rotated sprites. Do not use it for
fullscreen stills.

---

## 7. Input

- [x] `PAD_Init` + `rbo_input_poll` once per tick (`FUN_0043d350`)
- [x] Title menu local edges; Y dumps MEM1
- [~] `IDirectInputDevice8::GetDeviceState` snapshot only (no second `PAD_Read`)
- [ ] Fill DIJOYSTATE the way retail KeyConfig expects (type 2 slots)
- [ ] Keyboard DIK path only if a menu actually needs it
- [ ] `FUN_0043d350` real key-scan when devices exist

---

## 8. Audio

- [~] `DirectSoundCreate8` exists; `FUN_0043acf0` / `FUN_0043ecd0` skip
- [ ] `IDirectSoundBuffer8` + ASND/AESND
- [ ] `BGM.PAC` / `SE.PAC` payload or pre-extracted WAV on SD
- [ ] Title BGM, then in-match SE
- [ ] ARAM for streaming BGM

---

## 9. Lobby / chars / stage select

- [ ] `FUN_00406c70` Lobby.Img
- [ ] `FUN_00467f90` st_select.img
- [ ] `FUN_00444c80` `%s.dat` + `%s.fob`
- [ ] Multi-pad join (`rbo_input_any_holding_a`)
- [ ] Mode 3 tick `FUN_00454f60` / `FUN_00455730`
- [ ] Mode 6 tick `FUN_00468100` / `FUN_00468c60`

---

## 10. In-match / HUD / FOB

- [ ] Un-skip ENTER only with fail-safe loaders
- [ ] `FUN_00470090` HUD from SYSTEM.Img (atlas already on SD)
- [ ] Mode 4/5 GameMain `FUN_0040b490` `FUN_0040f860` `FUN_004090f0`
- [ ] Pause `FUN_0043a850` / overlay `FUN_004748f0`
- [ ] Attach proto FOB VM **or** ingest stage script runner; not both at once
- [ ] Char pose compositor (DAT sheets, not full PC RAM)

Fighting on hardware stays `RBO.DOL` until this rung.

---

## 11. Saves / config / arena

- [x] `FUN_00440b50` reads LocalConfig (partial)
- [x] KeyConfig load probe
- [~] Arena/rank “new” / “old skip” logs
- [ ] `*.sv4` / `KeyConfig.v4` / `*.rv4` round-trip
- [ ] Registry ADVAPI32 stays “not found”

---

## 12. Host traps (imports still red)

Only the ones that will actually be hit as modes fill in:

- [ ] `CoCreateInstance` (DirectShow)
- [ ] `DS.CreateSoundBuffer`
- [ ] `CreateThread` if DS mixer or movie needs it
- [ ] `BitBlt` / GDI if a path still uses a DC
- [ ] `MessageBoxA` (maps to trap today; title dialogs should not)

DD overlay/clipper/palette traps can wait until a log names them.

---

## 13. MEM1 / PE data

- [x] Named `DAT_*` BSS, no 33 MB hole
- [x] `host_mem1_log` (`MEM1 tag a# u# f#`, Y on title)
- [ ] `tools/pack_pe_data.py` initialized `.data` / `.rdata` (so
      `DAT_00497578` and sprite banks are not zero)
- [ ] Giant lifted buffers → SD/ARAM pointers
- [ ] One heavy atlas at a time once lobby/stage exist

---

## Halt stubs (`fun_stubs.c`)

These `host_trap` and spin until START → Swiss. Tick a box when the
real (or handwritten-safe) function replaces the stub.

Handwritten `FUN_00437330` returns 1 only for slot 1. That is why title
survives: mode-0 extras (`39580`, `0f9b0`, `33c90`, `3c170`, `37380`)
are gated off. Restoring retail timers will hit those stubs immediately.

### Boot / title / movie (next work)

- [ ] `FUN_00404ac0` mode `0xB` tick
- [ ] `FUN_004050e0` mode `0xB` draw
- [ ] `FUN_004055e0`
- [ ] `FUN_00478390` mode 2 menu FSM
- [ ] `FUN_004785e0` mode 2 present
- [ ] `FUN_0043e9f0` mode `0xC` movie tick
- [ ] `FUN_0043e830` `FUN_0043eaa0` movie helpers

### Lobby / stsel / ranking / extras

- [ ] `FUN_00454f60` `FUN_00455730` mode 3
- [ ] `FUN_00468100` `FUN_00468c60` mode 6
- [ ] `FUN_00401c20` `FUN_00403290` mode 7
- [ ] `FUN_00457bc0` `FUN_00458210` mode 8
- [ ] `FUN_00442070` `FUN_004422f0` mode 10
- [ ] `FUN_0045b860` `FUN_0045b900` `FUN_0045bcd0` `FUN_0045df90` `FUN_0045eee0` `FUN_0045f060` mode `0xD`
- [ ] `FUN_00424a50` `FUN_00424ac0` mode `0xE`

### In-match / GameMain

- [ ] `FUN_0040b490` `FUN_0040f860` `FUN_0040f9b0` `FUN_0040fb90` modes 4/5
- [ ] `FUN_0040f8a0` MessageBox / arena fail string

### Mode 0 tick extras (gated today)

- [ ] `FUN_00433c90` `FUN_0043c170` `FUN_00437380` `FUN_00439580`

### Other linked stubs (unknown / later)

- [ ] `FUN_004343d0` `FUN_00434b00` `FUN_004350e0` `FUN_004356d0` `FUN_004358a0` `FUN_004363a0`
- [ ] `FUN_004392e0` `FUN_00439db0` `FUN_0043a490`
- [ ] `FUN_0043b220` `FUN_0043b3c0` `FUN_0043b480` `FUN_0043b500`
- [ ] `FUN_0043f230` `FUN_0043f620` `FUN_0043f9c0` `FUN_00440290`
- [ ] `FUN_004446a0` `FUN_0044fce0` `FUN_00450780`
- [ ] `FUN_00466820` `FUN_00466880` `FUN_00466ab0` `FUN_0046a620` `FUN_0046aa50`
- [ ] `FUN_00471b00` `FUN_00473800` `FUN_00473c80` `FUN_00478970` `FUN_00478ac0`
- [ ] `LAB_0043f290` `LAB_0043fedf` `LAB_0043ff6c` `LAB_0043fff9` `LAB_00440082` `LAB_00440110`
- [ ] `LAB_00440a5c` `LAB_00440a96` `LAB_00440a9c` `thunk_FUN_00435aa0`

Ingest the callee. Do not leave a halt on a path you intend to survive.

---

## Not in the DOL at all

No object, no stub. Linker will name these when a caller is ingested.

- [ ] `FUN_00426c40` PAC basename → payload
- [ ] `FUN_00404960` `FUN_00404890` `FUN_00404850` `FUN_00404840` boot enter
- [ ] `FUN_004042d0` `FUN_00404380` `FUN_00404400` `FUN_00404540` boot FSM
- [ ] `FUN_00477770` `FUN_004777e0` `FUN_00477ab0` `FUN_004775f0` retail title
- [ ] `FUN_00413d40` retail D3D playback (`__ftol`)
- [ ] `FUN_00406c70` Lobby.Img
- [ ] `FUN_00467f90` st_select.img
- [ ] `FUN_00444c80` char `.dat` + `.fob`
- [ ] `FUN_00453f70` DirectShow graph
- [ ] `FUN_0043a850` pause
- [ ] `FUN_004748f0` pause overlay

Plus ~3000 other `FUN_*` that have never been demanded by the linker.

---

## Quiet handwritten keepers (leave until battle)

These are `/* wholesale-host */` no-ops or empty-list walks. Title needs
them to return. Do not ingest retail until the list heads are real.

- [~] `FUN_00403d40` map/bg 3D (count 0)
- [~] `FUN_0046e570` `FUN_0046e5c0` `FUN_0046b470` afterimage / projectile / spark
- [~] `FUN_00433ca0` follow-cam
- [~] `FUN_0043b870` nameplates
- [~] `FUN_00470730` `FUN_00474d40` `FUN_00475c90` in-game HUD
- [~] `FUN_00473f20` logo/fade (slot 0)
- [~] `FUN_00437330` tick-slot gate (only slot 1 runs)

---

## Suggested order (when we resume)

**Primary path** is title → lobby (mode 4) → stsel (mode 6) → stage 1
(mode 0 as play). Plan and checkoff:
[WHOLESALE_PLAY_PATH.md](WHOLESALE_PLAY_PATH.md).

Boot plates (`0xB`), retail mode 2 Title.Img, movie, and ranking are
not on that path. Leave them here until the WinMain loop survives lobby
and stage 1.

Until Gate 3, fight on `RBO.DOL`.
