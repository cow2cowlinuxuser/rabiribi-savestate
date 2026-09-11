# Play path: title → lobby → stsel → stage 1

This is the wholesale success path. Boot plates, movie, ranking, and
retail mode-2 Title.Img are **not** on it. They stay on
[WHOLESALE_CHECKLIST.md](WHOLESALE_CHECKLIST.md).

If lobby cannot stay in the WinMain loop, or stage 1 cannot be entered
without `DAT_00bc91a4 = 0xff`, `RBO_EX3.DOL` is a title demo. Fighting
on `RBO.DOL` does not count as this path working.

Tick a box when that gate is live on hardware.

---

## Why this path

The game is the mode loop in `FUN_00440690`:

```
DAT_00bc91a4 = next mode
while (DAT_00bc91a4 != 0xff) {
    DAT_00bc91a0 = DAT_00bc91a4
    FUN_0043f750(0)          // enter; 0 return → 0xff → loop dies
    while mode unchanged:
        FUN_0043d350()       // pad (once per tick)
        FUN_0043fab0()       // tick + draw
    FUN_0043f9c0(0)          // leave
}
```

Title on Cube today never leaves that loop. A on Start only logs
`MENU start`. The port dies the first time enter returns 0, a halt stub
runs, or MEM1 walks a PE image-end sentinel.

---

## Naming trap (do not ingest the wrong screen)

Our stand-in title occupies **mode 0**. Retail mode 0 is **stage play**,
not title.

| What we say | Coarse mode `DAT_00bc91a0` | Enter | Tick / draw | Plate |
|-------------|---------------------------:|-------|-------------|-------|
| Title (now) | 0, forced | skips + stand-in `73890` | `3c690` → `3c610` → `73890` | Title.Img stand-in |
| Title (retail) | **2** | `77770` `777e0` | `78390` / `785e0` | Title.Img |
| **Lobby** (charsel) | **4** | `09020` `090f0` | `0b490` / `0f860` | `CharaSel\Lobby.Img` |
| Ranking hub | 3 | `54cf0` `54d70` | `54f60` / `55730` | `Ranking\Ranking.Img` |
| Stage select | **6** | `67f60` `67f90` | `68100` / `68c60` | `StageSelect\st_select.img` |
| **Stage 1** | **0** (again) | `3bd00` `6c1c0` `3be20` | `3c690` / `3c610` | STAGE01.FOB + SYSTEM.Img |

Retail Start Game (`FUN_004775f0` case 0) returns 2. Mode-2 tick maps
that to **mode 4**, not mode 3. Mode 4 enter `FUN_004090f0` is the
function that loads Lobby.Img (`FUN_00406c70`). Mode 3 loads Ranking.Img.

Stsel confirm (`FUN_00468100` when the inner FSM hits 3) returns 2.
Mode-6 tick maps that to **mode 0**. That is stage play. Our title
stand-in must be **off** before we come back.

Do not ingest mode 3 for "lobby".

---

## Pass rule (every gate)

A screen is live only if all of these hold on Cube:

1. Enter does not set `DAT_00bc91a4 = 0xff`.
2. Tick and draw run every frame. No `stub_halt`.
3. A advances to the next mode. B returns to the previous one.
4. `MEM1 tag` on Y stays sane. No 33 MB PE hole.
5. START still Swiss.

Missed files log and return 1 until that screen can draw with what it
has. Return 0 is how the port dies.

---

## Gate 0 — leave title without dying

Stand-in `FUN_00473890` stays. Do not switch to retail mode 2 for this path.

- [x] A on Start sets `DAT_00bc91a4 = 4` (not a log-only).
- [x] B / Exit may stay on title. Ranking stays off this path.
- [x] Stop forcing mode 0 in `FUN_0043f750`. Honor `DAT_00bc91a4`.
- [x] `3f750` implements **0, 4, 6 only**. Any other mode logs and
      sets `0xff` only if we actually jumped there by mistake.
- [x] Leave `FUN_0043f9c0`: stub per mode. **Free the eight title BGs**
      when leaving 0. That MEM1 is the lobby budget.
- [x] Gate stand-in `73890` so it does not draw once we are no longer
      on the title stand-in (mode 0 will mean stage later).
- [x] `FUN_00437330`: return 1 for slots 0 and 1 while in 4/6 (and
      later real 0). Keep slot 2 off (`39580` / `0f9b0` still halt).

Do not ingest full retail `3f750` until mode-4 enter is fail-safe.

---

## Gate 1 — lobby (mode 4)

Enter retail: `FUN_00409020` then `FUN_004090f0`:

`0f990` → `03d20` (map/bg) → `44f80` (chars) → `06c70` (Lobby.Img +
c_create + skill_name loop) → `75500` (256×256 surfaces, PE end
`0x256b728`) → `6c2f0` (SYSTEM.Img + Hit_Effect).

Tick: `FUN_0040b490`. Draw: `FUN_0040f860` (uses `FUN_0040fa50`, **not
in the DOL**). Cancel: `FUN_00402040` → `FUN_0043dab0(0xe)`.

- [x] Loose `sdc:/RBO/DATA/CG/CharaSel/Lobby.Img` (or TPL stand-in).
      Do not 8.3-rename.
- [x] Handwritten `090f0`: load Lobby.Img via `FUN_00476ed0`. Skip
      `c_create01/02` and `skill_name_%02d` until the plate is up.
- [x] Handwritten `06c70` subset. `FUN_00476e70` needs **PE 0x28 sprite
      banks** (`DAT_0048a230` …). Zeros = blank quads. See PE section.
- [x] Do **not** ingest `FUN_00444f80` (3 party + 40 + extras `.dat`/`.fob`).
      Empty slots (`DAT_00bd3fb0[i] == -1`) already skip in `44e60`.
      Seed three `-1`s. One dummy char later, not 43 packs.
- [x] Do **not** ingest `FUN_00475500` PE surface loop. Cap 0–3 surfaces
      or skip until portraits exist.
- [x] Skip `FUN_00403d20` map/bg (title already used count=0).
- [ ] SYSTEM.Img may load here or stay skipped. Lobby plate first.
- [x] Handwritten `0f860`: blit Lobby.Img dests from
      `EXTRACTION/dumps/ui_sprites/DEST_LAYOUT.md` (same packet + AABB
      path as title). Implement or alias `FUN_0040fa50` → current
      `0fa70` playback.
- [x] Handwritten `0b490`: A → `DAT_00bc91a4 = 6`. B → `= 0` back to
      stand-in title (or 2 if we have moved title). No halt.
- [ ] `FUN_0043dac0` / `3da00` / `3dab0` / `36f30`: pad bits from the
      **already-polled** snapshot. Do not `PAD_Read` again.

Lobby is live when we see CharaSel art, A goes to stsel, B returns to
title, loop still running.

---

## Gate 2 — stage select (mode 6)

Enter: `FUN_00467f60` then `FUN_00467f90`:

`st_select.img` + UV banks `DAT_0049c4a0` … then `st_sel_map.img` +
`FUN_00467770`.

Tick `FUN_00468100` walks `DAT_02553b80` and tests
`DAT_0049ceac[stage*2] & 4` (unlocked). Confirm returns 2 → mode 0.

- [x] Loose `st_select.img` + `st_sel_map.img` on the card (or `STSEL.TPL` fallback).
- [x] Handwritten `67f90`: load both (map/chip can miss). Fail-safe.
- [x] Pack or seed `DAT_0049ceac` so **stage 1 bit 4 is set**. BSS zeros
      make the cursor loop forever. *(tick treats stage 1 as unlocked.)*
- [x] Handwritten `68100`: D-pad among unlocked, A confirm →
      `DAT_00bc91a4 = 0` and store stage index (`FUN_00433fe0` /
      `FUN_00467e40` or a host dword). B → mode 4.
- [x] Handwritten `68c60` blit. Dests in DEST_LAYOUT STSEL.
- [x] Leave frees lobby atlas before stsel atlas if MEM1 is tight.

Stsel is live when the map/plate draws, stage 1 is selectable, A leaves
to mode 0 without halt.

---

## Gate 3 — stage 1 (mode 0 as play)

Retail enter: `FUN_0043bd00` (replay + `FUN_00434160` Stage_XX.fob +
`FUN_00434220` bind) → `FUN_0046c1c0` unit tables → `FUN_0043be20`
SYSTEM/result/chars.

`3bd00` returning 0 is the TITLE skip reason. That skip **blocks stage
1**, not the title screen.

- [ ] Stand-in `73890` does not draw. Mode 0 is play.
- [ ] `3bd00` fail-safe: load `STAGE01.FOB` (ETC.PAC or loose). Miss
      logs, still return 1 only if we have a host fallback (proto VM).
      Return 0 still kills the loop.
- [ ] `6c1c0`: named BSS zero of **bounded** party/unit tables. Do not
      ingest Ghidra image-end loops.
- [ ] `3be20`: fail-safe. SYSTEM.Img already in host. No 43-char blast.
- [ ] Tick slot 0 must run GameMain-ish update. Slot 1 draws `3c610`
      **without** the title overlay.
- [ ] Un-skip HUD `FUN_00470090` only after packet pool + SYSTEM slices
      are proven (title already has the atlas).
- [ ] Attach proto FOB VM (`STAGE01.FOB` FULL path) **or** ingest
      `FUN_00427c70` / `FUN_0042bf60`. Not both at once. First pass:
      proto VM under wholesale Flip, so the mode loop is real and
      something walks on screen.
- [ ] BG plates: proto STAGE01.TPL / baked walkscape. Do not decode
      six 1024 BG IMGs on top of lobby leftovers.
- [ ] Chars: proto sheets for the Stage 1 roster, not `FUN_00444c80`
      PE compositor.

Stage 1 is live when we leave stsel, stay in the loop, and see stage
art (even a walkscape + one actor). Full combat can still be proto
natives.

---

## Big MEM (do these or lobby/stage OOMs)

MEM1 is 24 MB. Title already keeps eight 640×480 stills plus Title.Img
plus the RT.

- [ ] Free title BGs on leave (Gate 0). Log `MEM1 tag` before/after.
- [ ] One heavy atlas resident: Lobby **or** stsel **or** stage BG.
- [ ] Never ingest PE-sized loops:
      - `FUN_0046c1c0` unit zero (image-end sentinels)
      - `FUN_00475500` to `0x256b728`
      - `FUN_00444f80` 43 char packs
      - `FUN_00435750` 633 KB ranking (already skipped)
      - `FUN_00434f10` ArenaRecord PE VA `0xae0ca8`
- [ ] Char `.dat` is a pointer to SD/ARAM, not a BSS clone of PC RAM.
- [ ] `host_mem1_log` on every mode enter/leave, not only Y on title.

If leftover KB after lobby enter is worse than title-after-BG-free,
stop and cut surfaces before ingesting more C.

---

## PE data (sprite banks, not the 33 MB hole)

`tools/pack_pe_data.py` is named in WHOLESALE_DOL.md and **does not
exist**. `collect_dat_symbols.py` only lists names.

Ghidra BSS zeros mean `FUN_00476e70` stamps width/height onto **empty**
`0x28` records. Lobby/stsel/HUD dests live in EXE `.data`.

- [x] Write `pack_pe_data.py`: copy **initialized** PE `.data` / `.rdata`
      bytes for the banks this path touches, emit a `.o`. BSS stays
      named `DAT_*` packing. Never map VSz 34498168.
- [x] Banks for lobby: `DAT_0048a230` and the `FUN_00406c70` list.
- [ ] Banks for stsel: `DAT_0049c4a0` `0049c548` `0049c4d0` `0049c508`
      `0049c580` `0049c468`. Unlock table `DAT_0049ceac`.
- [ ] Banks for HUD later: `DAT_0049d128` …
- [ ] Seed `DAT_00bd3fb0[0..2] = -1` (empty party) until a real pick.
- [ ] `DAT_00497578` stays 0 for this path (we force 4 from the
      stand-in). Do not seed 11 unless boot plates come back.

Dump already mined: `EXTRACTION/dumps/ui_sprites/` and DEST_LAYOUT.md.
Prefer packing those records over ingesting `76e70` against zeros.

---

## PC handlers (only what this path will call)

Poll pad **once** in `FUN_0043d350`. Everything else reads the snapshot.

- [ ] `FUN_0043dab0` / `3dac0` / `3da00` / `3da40` / `36f30`: button
      bits (A/B/D-pad). Ghidra typed `36f30` as void; stsel uses the
      return. Handwritten.
- [ ] `FUN_00402040`: B/back → 3 so GameMain tick can leave to title.
- [ ] `FUN_00479e80`: already pumps; keep it non-zero.
- [ ] `FUN_0043f9c0`: leave 0 / 4 / 6. Stub 4→`09150`, 6→`680b0` as
      Release surfaces + log. Missing leave leaks MEM1.
- [ ] `FUN_0040f990`: BeginScene/clear (title already clears in device).
- [ ] `FUN_0040fa50`: alias to current packet playback or halt on first
      lobby draw.
- [ ] `MessageBoxA`: must **not** `host_trap` if a save/rank helper
      still returns 0. Return IDOK and log.
- [ ] DI `GetDeviceState`: enough buttons for `3dab0`. No second poll.
- [ ] `CreateThread` / `CoCreateInstance` / `CreateSoundBuffer`: still
      traps. Do not take a path that hits them (sound/movie stay skipped).

---

## Assets on the card (this path)

| Screen | PC path | Status |
|--------|---------|--------|
| Title | `CG\Title\Title.Img` + Title_Bg 001–008 baked | on card |
| Lobby | `CG\CharaSel\Lobby.Img` | extracted to `sd_pack` (copy to card) |
| Stsel | `CG\StageSelect\st_select.img` + `st_sel_map.img` | IMG or `STSEL.TPL` fallback |
| Stage 1 | `STAGE01.FOB` + walkscape TPL (proto) | proto tree; copy or share |
| HUD | `CG\GameMain\SYSTEM.Img` | title already loads it |

PAC payload (`FUN_00426c40`) can wait until loose IMGs prove the
screens. Then put CharaSel/StageSelect in `CG.PAC`.

---

## Suggested implementation order

1. **PE banks + pack script** for lobby/stsel 0x28 records and
   `DAT_0049ceac`. Without this, plates load and draw black.
2. **Gate 0** mode switch + free title BGs + `37330` slots 0+1.
3. **Lobby plate only** (no chars, no 75500). A/B mode change.
4. **Stsel plate** + stage 1 unlocked + A to mode 0.
5. **Mode 0 play**: kill stand-in overlay, fail-safe `3bd00`, proto FOB
   VM + STAGE01 art.
6. Then: one party slot, HUD un-skip, PAC 26c40, sound.

Do not start Gate 3 until Gate 1 holds a frame on hardware. A black
lobby that still loops is a win. A halt on `0b490` is a dead port.
