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

Counts after cycle 4: **save_slot 310**, **resume 326**, **fault 0**.
Mixer tid 500 still parked from load 8. 49 threads. Slot still 173
regions / ~523 MB.

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
