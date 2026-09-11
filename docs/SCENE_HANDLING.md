# Scene handling model (PC → DOL)

Basis: [RE_FINDINGS.md](RE_FINDINGS.md) mode FSM + live proto boot chain.

## Fine scenes (GC / DOL)

Owned by `rbo_scene` (`RboSceneId`). Numeric order is stable:

```
CAUTION → LOGO → INTRO → TITLE → LOAD → LOBBY → STSEL → PLAY
```

| Id | Name | Assets resident | Input notes |
|----|------|-----------------|-------------|
| 0 | CAUTION | TITLE.TPL (cscreen) | A / timer skip |
| 1 | LOGO | TITLE.TPL (Boot_logo ×3) | A / timer advances each studio logo |
| 2 | INTRO | TITLE.TPL (iscreen) | A/B skip (movie stub) |
| 3 | TITLE | TITLE.TPL (title_atlas slices) | Menu; START→Swiss |
| 4 | LOAD | none / leftover title until swap | Timed |
| 5 | LOBBY | LOBBY.TPL (atlas + create) | Slots; **any pad hold/press A** to join |
| 6 | STSEL | STSEL.TPL (atlas + chip01) | D-Pad LEAN/FULL; A→PLAY; B→LOBBY |
| 7 | PLAY | STAGE01.TPL (+ optional STAGE01.FOB on FULL) | Move / A attack / **B→STSEL** / **START→pause** (Lobby / Title / Exit→Swiss); RESET→Swiss |

## Coarse PC modes

`rbo_scene_to_mode()`:

| Fine | PC `DAT_0256bd30` |
|------|-------------------|
| CAUTION, LOGO, TITLE | 0 TITLE |
| INTRO | 1 OPENING |
| LOAD, LOBBY, STSEL | 3 HUB |
| PLAY | 5 BATTLE |
| (future RESULT) | 4 RESULT |

## Memory rule (non-negotiable)

One heavy pack at a time:

1. Unload UI TPL before STAGE01.
2. Unload STAGE01 before returning to STSEL/LOBBY/TITLE (B or pause menu).
3. FULL path: malloc `RBO/SCRIPTS/STAGE01.FOB` only on STSEL confirm; free with stage unload.
4. `DCFlushRange` after SD load (`rbo_dma` / `rbo_mem`).

## Pause (PC `FUN_0043a850`)

Skip first overlay (`FUN_00474870`). Open 3-item menu (`FUN_004748f0`) on START:

| Sel | PC return | GC action |
|-----|-----------|-----------|
| 0 RETRY LOBBY | 2 | unload STAGE01 → LOBBY (DEPART) |
| 1 RETURN TITLE | 3 (+ `FUN_004087a0` party reset) | unload → TITLE.TPL |
| 2 EXIT GAME | 4 | Swiss |

## Transition API

```c
rbo_scene_goto(RBO_SCENE_TITLE);   /* updates current id */
rbo_scene_current();               /* read */
rbo_scene_to_mode(id);             /* PC coarse mode */
```

Asset side effects stay in `main.c` / `rbo_assets` for now; `goto` only tracks identity so Dolphin/hardware can verify control flow without silent pack leaks.

## Draw contract per scene

| Scene | Draw |
|-------|------|
| Boot / title / lobby / stsel | `rbo_gx_blit_fullscreen(tex)` + HUD glyphs |
| LOAD | solid clear + text |
| PLAY | stage BG → Y-sorted actors → HUD → present |

Present = existing CopyDisp + `rbo_gx_present()` (VSync / Flip stand-in).

## Lobby multi-pad (PC-like)

PC warns when not enough pads are configured. GC model:

- `rbo_input_poll()` all four chans
- Greet advance: `rbo_input_any_pressed(PAD_BUTTON_A)` or holding A
- Future: count connected pads holding A as party size

## Test plan

1. **Dolphin:** boot Caution→…→Title→Lobby→Stsel→Stage1; Swiss on START.
2. **Hardware:** same via sdpush; watch MEM% on PLAY.
3. If scene id desyncs from drawn assets, fix callers of `rbo_scene_goto` before adding more packs.
