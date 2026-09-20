---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Cadence experiment: one save + five restores / 5 min

Live linux **208448** (GPU=3 TEX_SCALE=1). Rapid pair loop stopped at
**resume 306** / **save 306**; continue **209232** killed (game left
alone). Driver: `cadence-save5.sh` **247198**, 24 cycles × 300 s.

## Cycle log

| cycle | time | save # | restores | visual after r5 | wrapper |
| 1 | 19:41 | 307 | 311 | in-world beach HP 68 | 0 fault, mapid 1, 4 heaps UNCHANGED |
| 2 | 19:46 | 308 | 316 | CONTINUE? | same, 0 fault — idle death, not a bad copy |
| 3 | 19:51 | 309 | 321 | CONTINUE? | save/restore of overlay; still 0 fault |
| 4 | 19:56 | 310 | 326 | in-world beach HP 78 | after YES at 19:54; new save did not disrupt |
| 5 | 20:01 | 311 | 331 | in-world HP 78 | 0 fault |
| 6 | 20:06 | 312 | 336 | in-world | 0 fault |
| 7 | 20:11 | 313 | 341 | in-world HP 50 | 0 fault |
| 8 | 20:16 | 314 | 346 | in-world HP 88 | 0 fault |
| 9 | 20:21 | 315 | 351 | CONTINUE? | idle death; YES at 20:22:58 → in-world HP 110 |
| 10 | 20:26 | 316 | 356 | in-world HP 1 | living save after YES |
| 11 | 20:31 | 317 | 361 | in-world ~347 KB | 0 fault |
| 12 | 20:36 | 318 | 366 | in-world ~348 KB | 0 fault |

Counts after cycle 12: **save_slot 318**, **resume 366**, **fault 0**.
Mixer tid 500 still parked. Cadence **247198** still running toward 24.

## Does a new save at 5 min disrupt session state?

**Not so far.** Each KEY_1 overwrites slot 0 with whatever the live
session is. Five KEY_2s restore that snapshot. Held-hash verdict stays
UNCHANGED; mapid stays 1; no `fault:`.

What *does* change the picture is the **5-minute live idle**: beach
enemies killed her between cycle 1 and 2. The next save then correctly
captured CONTINUE, and five restores correctly put CONTINUE back.
Clicking YES (19:54) returned in-world HP 108; cycle 4 saved that
living state (HP 78 after two minutes of contact) and five restores
left her on the same umbrella platform.

So: periodic new saves are copying true game state, not scrambling it.
Longevity of the *in-world* view still depends on not dying during the
idle, which is gameplay, not the wrapper.

Cadence left running toward cycle 24. Do not kill 208448.
