# Stage 1 chain (PC → GC)

There was **no** prior end-to-end design doc for Stage 1 spawns / NPCs / events.
This file is the working chain map, built from `STAGE01.FOB` + Ghidra VM RE.

## Source of truth

| Artifact | Path |
|----------|------|
| Retail script | `ETC.PAC` → `STAGE01.FOB` (131076 B) |
| Patch script | `Update01.PAC` → `STAGE01.FOB` (131654 B) |
| Extracted | `EXTRACTION/dumps/scripts/stage01/` |
| Loader | `FUN_00427c70` (LoadScript) |
| Opcode VM | `FUN_0042bf60` (u16 ops `0…0x19`) |
| Native host | `FUN_0042afe0` (opcode 1 — large) |
| Stage bind | `FUN_00434110` / `00434220` / `004340e0` / `00437b90` |

Extract: `powershell -File tools/extract-stage01-fob.ps1` (repo root =
`Ragnarok Battle Offline + Ex1-3`).

## FOB layout

```
u32 n_exports
Export[n] { char name[0x20]; u32 code_offset; }   // 0x24 each
u32 n_pools                                         // STAGE01 = 0
…pools…
u32 bytecode_len
u8  bytecode[bytecode_len]
```

`code_offset` is relative to the start of `bytecode[]`.

## STAGE01 exports (entry points)

| Export | Role |
|--------|------|
| `SetLoadCharaList` | Build character load list for the stage |
| `StageInit` | One-shot setup (BG, BGM, caution, emotion pack, …) |
| `StageMainCallback` | Per-frame / tick while in battle |
| `StageEventCall` | Fired when event flag set (`FUN_00437b90`) |
| `StageBgmStopAll` / `CheckStageBgmStopAll` | BGM teardown helpers |
| `ReleaseStageScript` | Unload |
| `RankingStage*` | Ranking-mode variants (defer) |

## Asset chain (what the script references)

```
SetLoadCharaList
    └─ char IDs → DATA01/02 `.dat`+`.fob` (mobs / NPC_ALL / PRO_C_OBJ)

StageInit
    ├─ BG01 plates: bg01_01_01 … bg01_06_01 (+ 03_02 / 03_03 overlays)
    ├─ Emotion: Emo.Fob + set_emotion_layer_type
    ├─ Caution UI: stage_caution.img
    ├─ Field BGM: prowav01 / prowav02
    ├─ Boss BGM:  pro-boss01 / pro-boss02
    ├─ Ambient SE: ST01_bg_bird.wav
    └─ Clear BGM: clear / clear-koushin

StageMainCallback + StageEventCall
    ├─ Area / camera / spawn triggers (natives — not plain ASCII)
    ├─ Chat / Mes:%s (Shift-JIS in JP FOB)
    ├─ Boss gate (ENG patch names Eclipse; IDs via tables)
    └─ Clear / ranking handoff
```

Mob roster already in GC TPL: Poring, Equrips, Rocker, Creamy, Spore,
Fabre, Lunatic, Willow, ChonChon. NPCs / Emo / PRO_C_OBJ **not** packed yet.

## PC runtime order (battle mode 5)

1. Load `Stage%02d.fob` → bind `StageMainCallback`
2. Call `SetLoadCharaList` → load char packs
3. Call `StageInit`
4. Each frame: GameMain + `StageMainCallback`; on flag → `StageEventCall`
5. Exit / clear → `ReleaseStageScript`

## Offline miner (Path A — done)

| Tool | Output |
|------|--------|
| `tools/disasm-fob.py` | Per-export `.lst` under `EXTRACTION/dumps/scripts/stage01/disasm/` |
| `tools/mine-chara-ids.py` | `chara_id_names.csv` + `stage01_load_list.txt` |
| `tools/mine-stage-data-blobs.py` | DATA blobs from StageMain area calls |
| `tools/decode-stage-spawns.py` | `stage01_spawns_decoded.csv` → `rbo_fabre_proto/source/stage01_spawns.h` |
| `tools/mine-pekoknight.py` | `pekoknight_stage01_packs.csv` + `PEKOKNIGHT/disasm/pekoknight_cmd248.csv` |

**SetLoadCharaList roster (char_id → name):**  
Poring(25), Fabre(89), Lunatic(84), ElderWillow(92), Willow(85), Rocker(86),
Snake(87), ChonChon(88), Spore(90), Creamy(91), Equrips(93), Ambernite(94),
HunterFly(95), Kafra(117), PekoKnight(181), NPC_ALL(182).

**Static DATA spawns:** stride-13 records. Combat is `slot_id == char_id`
(Poring=25 at `@012F50`). `slot=NPC_ALL(182)` + `param=pose` is scenery.
A decoder bug treated pose 91/86/90/88 as Creamy/Rocker/Spore/ChonChon
because those numbers collide with char ids. HunterFly param=95 is the
same class of miss. Fabre/Lunatic/Equrips are **not** in the static blob.

## GC port status (PLAY)

| Piece | Status |
|-------|--------|
| STAGE01.TPL walkscape | Live — 5 plates, **BG_W=2048** (PC pixel X), STAGE_W=10240 |
| Parallax | mid/near **only on plate 2** (bg01_03_02/03) — not tiled |
| BG bake | `tools/bake-stage01-bg.ps1 -Preset console\|half` (texels stretch to 2048) |
| Spawns | FOB DATA placements only (no invented opener pack) |
| Novice attack stub | Live |
| Pause menu | Live (Lobby / Title / Swiss) |
| Battle HUD | Live — 3 party slots (center occupied), follow HP/SP, radar strip, STAGE/AREA/TIME |
| Scenery NPCs | Live — 10 plate-0 NPC_ALL idle poses (quarter of 38 DATA rows); compact atlas |
| FOB VM on console | **FULL path only** — STSEL → STAGE 1 FULL; lean unchanged |
| Script cues imported | hi-gate locks (contiguous), aggro chase, CLEAR→clear.ogg cue |
| FULL BGM | `rbo_audio_poll` + yield inside VM so field OGG can loop |
| Real events / boss / bird SE | Missing (natives stubbed on FULL) |
| Death / ST01 SE sheets | Missing |
| Elder Willow midboss | Scenery-layer tree (PC Area 3); EventCall id 92 — no sheet yet |
| Peko Knight midboss | Real fight (died here on PC). DAT=sprites, FOB cmd 23 summons Porings. No GC sheet yet |
| HunterFly | Load-listed only — not a Stage 1 field spawn (PC playthrough) |

MEM1 on hardware ~70% with the previous STAGE01 pack. HUD chrome is already
paid (`system_atlas` 1024 RGB5A3). Extra this pass: NPC_ALL compact
512×1024 RGB5A3 (~1 MB; CMPR punched black boxes) plus ~10 scenery actors.
chrome is a SYSTEM slice). HUD `MEM % %uK` is **arena free** (updates every
frame; stays flat in PLAY unless we alloc). Cut `NPC_SCENERY_MAX` if MEM%
jumps more than a few points.

BG strategy (console-first): **world X = PC plate pixels** (`BG_W=2048`,
`STAGE_W=10240`). Bake texels at 640×320 or 1024×512 and stretch. Do not tile
`03_02`/`03_03` outside plate 2. `bg01_06` is a 256² prop. Stream 2–3 plates
later if MEM% climbs.

## Section / area flow (GC)

PC: `StageMainCallback` SWITCH `@12098` → `CALL area_*` (a@75296, b@78400,
c@81972, d@86176, …). Nested SWITCH `@12118`. `HOST_C.1` → `FUN_00438110`
sets scroll/X lock `(lo, hi)`. Mined bands (`tools/mine-host-c1.py`):
`2048..4096`, `0..5120`, `5120..7168`, `6144..8192`, `8192..9216`, …

Retail is one contiguous strip: clear a pack **raises `lock_hi`**. The player
is not teleported onto the next plate. `apply_gate_lock` never raises `lock_lo`
past `novice.x`. Camera follows and only stops at the current hi gate.

Kill→advance native still opaque; EventCall still owns some openers
(Lunatic / Equrips / ElderWillow / Ambernite ids in HOST_11/D, no coords).

GC (`stage_section.h` + `stage01_spawns.h` + `main.c`):

| Sec | Kind | FOB CALL | Gate hi | Spawns (DATA) |
|-----|------|----------|---------|---------------|
| 0 | FIELD | area_a 75296 | 5120 (`HOST_C.1` 0..5120) | 6× Poring@4340–4720 (same AREA 1 as town) |
| 1 | FIELD | area_b 78400 | 4096 (`HOST_C.1`) | none (raise-only; hi already 5120) |
| 2 | FIELD | area_c 81972 | 5120 | none (pack already in sec 0) |
| 3 | CLEAR | — | 10240 | none (walk plates 3–4; mid Peko fight is past this) |

PC recording (died at Peko Knight): Area 1 is area_a — town NPCs **and**
the first Porings (`@012F50` at x≈4340). area_a has no HOST_C.1; a fake
2048 plate-0 gate trapped GC on the brick wall with no enemies. First hi
is 5120 so the player can walk the wall into that pack while HUD stays
AREA 1. Later: Area 2 Willows; Area 3 Fabres/Porings + **Elder Willow**
(tree until it fights); Area 4 Rocker + Poring swarm + **Peko Knight with
trailing Porings**.
HunterFly never appears. Fabre is EventCall-spawned (not in static DATA).
Kafra@400 is its own char (slot 117), not NPC_ALL. Lean and FULL share this
table; FULL natives are still stubs so the two paths look the same in PLAY.

### Peko Knight pack (mined)

Two Stage 1 appearances, plus extra Porings created by his own DAT/FOB:

| Pack | Where | Knight | Porings |
|------|-------|--------|---------|
| Early event | DATA `@012F50` ~x=4520 | id 181 pose **209** (序盤 生成) | **6** at 4340/4370/4520/4600/4670/4720, pose **220** (ペコ騎士イベント用 登場) |
| Mid fight (died here) | EventCall trigger **8292..9116** (lock 8192..9216) | pose **200** (中盤 生成), later 202/231; X check 10540 | DATA `@018E34`: **count=6**, pose **210**, offsets **-500..+600** around template X **15760** |

`PEKOKNIGHT.DAT` is the sprite/pose bank (`peko_knight.bmp`). The trail logic
lives in **`PEKOKNIGHT.FOB`**: command-248 type **23**, three variants (0/1/2),
each summons **char_id 25** (Poring). That is why more than 6 Porings show up
once he starts attacking. `HOST_11.0` in EventCall also queries id 25.

Template X 15760 is just past `HOST_C.1` 14336..15360. Retail Stage 1 is
wider than GC `STAGE_W=10240`. Do not treat the plate-2 Porings as the mid
fight, and do not ship the knight until a sheet is in the TPL.

Miner: `tools/mine-pekoknight.py`.

### Battle HUD (GC)

`rbo_hud.c` reimplements the visible PLAY chrome from SYSTEM.Img (not the
4k-line PC compositor). Dest/UVs: `ui_layouts.h` + `DEST_LAYOUT.md`.

| Piece | PC | GC |
|-------|----|----|
| 3 party frames | `FUN_00470730` / `DAT_0049d178` | Always drawn; 1-player fills **center slot 1** |
| Empty chrome | `0x0049D150` src 230,4 + Empty... strip | Slots 0 and 2 |
| Follow HP/SP | `FUN_0046f570` | Same fills, offset from novice screen pos |
| Radar | `FUN_0046bd80` / `spot.img` | SYSTEM slice 124,252,264×56 + X-mapped blips |
| Top bar | `FUN_0046d5e0` | `STAGE 1 - AREA n`, `TIME mm'ss"cs`, item `x00` stub |
| MEM% | (PC FPS) | Arena-free debug; FULL path adds `FULL PH` above it |

Bottom name bar at (18,462) is not drawn (slot text owns the nameplate).
DAMAGE AVE is a constant 0 stub.

### Scenery NPCs (quarter)

Do **not** load `NPC_ALL.DAT` (10 MB). Export:
`tools/export-npc-scenery.ps1` → `npc_all_sheet.png` + `npc_all_anims.h`.

10 plate-0 DATA `@01265C` rows (`x < 2048`), recording cluster first.
`param` is the DAT pose index; compact atlas order is the table in
`stage01_npcs.h`. Idle only, no aggro/HP, Y-sorted with everyone else,
cull if `|x - cam_center| > BG_W`. Y = `ground_y + y_off` (not floor-clamped).

| x | y_off | facing | DAT pose | Compact | Name |
|---|-------|--------|----------|---------|------|
| 180 | -45 | +1 | 30 | 0 | NOVI_A |
| 250 | -80 | +1 | 45 | 1 | ARC_M_1 |
| 310 | -35 | +1 | 75 | 2 | ARCHER_F_2 |
| 400 | -90 | +1 | 50 | 3 | MAGI_M_1 |
| 450 | -90 | +1 | 60 | 4 | PATTERN059 |
| 500 | 50 | +1 | 65 | 5 | MARCH_M_1 |
| 580 | -70 | -1 | 15 | 6 | PATTERN015 |
| 750 | 10 | -1 | 20 | 7 | PATTERN020 |
| 850 | 30 | +1 | 26 | 8 | PATTERN025 |
| 950 | -30 | -1 | 31 | 9 | PATTERN001 |

`NPC_SCENERY_MAX` (default 10) is a one-define cut; 19 later for half of 38.

- `area_d+` combat still deferred; plates 3–4 (`bg01_04/05`) are walkable after CLEAR.
- Actor pool = `ENEMY_POOL` (8), recycled per section.
- Clear section combat → `enter_section(n+1)` (MIDBOSS/BOSS kinds reserved).
- Novice clamped to `g_lock_lo..g_lock_hi` on **X only**. Live Y is pinned
  (`FLOOR_Y=448`, PC compositor origin after project). NPC/enemy `y_off`
  is FOB DATA word 9 (depth lane). `HOST_C.1` pushes
  `[17, 0, lo, 0, hi, 0]` — the Y halves are 0.
- Sprite size: pose `dest_w`/`dest_h` are the 640×480 pixel sizes from the
  DAT. Part `x_scale`/`y_scale` is walk/combat squash. Actor extra scale
  is **1.0** (`CHAR_SCALE_DEFAULT`) for novice, enemies, and scenery.
  Y still cycles 1 / 1.5 / 2 on all sprites for A/B.

## Wave / area flow (GC) — retired

Replaced by section rail above (was 3 curated waves @ 1600/4000/5120).

## Wholesale scripts? (decision)

**Experiment enabled:** STSEL → **STAGE 1 FULL** loads `RBO/SCRIPTS/STAGE01.FOB`
(~128 KB) into MEM1 on confirm only. Lean path unchanged.

Caveats (expected):

1. Opcode loop is small; most `HOST_*` natives are **stubs** (stack drained).
   `HOST_C.1` applies X locks as **hi gates** (no teleport).
   `HOST_D.0/1/2/4` run the nested spawn VM (`FUN_00425540`) over DATA
   payloads. Cmd-13 records with a resident sheet (Poring / NPC_ALL compact
   poses, etc.) go into the live pools. No `NPC_ALL.DAT` load. Caps stay
   `NPC_SCENERY_MAX=10` and `ENEMY_POOL=8`. Lean still uses mined tables.
2. Authentic Stage1 still needs remaining natives + char FOBs.
3. If malloc/`StageInit` fails: bounce back to STSEL with `FOB LOAD FAIL`.
4. HUD on FULL: `FULL PH n H hits S emits` (phase @12098, host calls, cmd-13).
5. VM calls `rbo_audio_poll` every 256 insns so field BGM can loop.
6. Dolphin: Slot A SD image, `RBO/` at the volume root (`sda:/RBO/...`).
   Hardware SD2SP2 remains `sdc:/RBO/...`. `SD ERR n` is drawn with VAT+ortho
   already set (1 = no volume, 3 = TITLE.TPL missing).

Lean (default) remains the MEM1-safe mined section rail.

## Recommended build path (MEM1-safe)

1. **Offline FOB miner** (PC): disassemble with operand sizes from
   `FUN_0042bf60` handlers; focus `SetLoadCharaList` + `StageInit` +
   `StageEventCall` → real spawn / trigger tables.
2. **Replace** curated `stage01_spawns.h` with mined data.
3. **Small FSMs on GC**: camera-X gates, BGM cue swaps, caution banner,
   despawn-on-death — no full VM (LEAN path).
4. **FULL path**: optional STSEL entry; allocate FOB only on load; unload on exit.
5. **Gameplay + scenery NPCs**: kill + ST01 SE + aggro for the 9 resident
   types; plate-0 NPC_ALL quarter is packed (raise `NPC_SCENERY_MAX` later).
6. **Boss last**: Eclipse stub (pos + HP + boss BGM) once field feels real.
7. **Stream later**: extra BG plates / Emo only if MEM% allows (ARAM/SD).

## Related docs

- [RE_FINDINGS.md](RE_FINDINGS.md) — mode FSM, PAC XOR, GameMain
- [SCENE_HANDLING.md](SCENE_HANDLING.md) — fine scenes + pause
- [PORT_SKELETON.md](PORT_SKELETON.md) — module map / memory rules
