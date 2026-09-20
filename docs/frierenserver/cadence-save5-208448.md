---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Cadence experiment: one save + five restores / 5 min

linux **208448** (GPU=3 TEX_SCALE=1). Rapid pair loop stopped at
resume **306**; continue **209232** killed (game left alone). Cadence
driver `cadence-save5.sh` **247198**, 1 save + 5 restores / 300 s.

## Cycle log

| cycle | time | save # | restores | visual after r5 | wrapper |
| 1 | 19:41 | 307 | 311 | in-world beach HP 68 | 0 fault, mapid 1, UNCHANGED |
| 2 | 19:46 | 308 | 316 | CONTINUE? | idle death; overlay copy UNCHANGED |
| 3 | 19:51 | 309 | 321 | CONTINUE? | still 0 fault |
| 4 | 19:56 | 310 | 326 | in-world HP 78 | YES at 19:54 |
| 5 | 20:01 | 311 | 331 | in-world HP 78 | 0 fault |
| 6 | 20:06 | 312 | 336 | in-world | 0 fault |
| 7 | 20:11 | 313 | 341 | in-world HP 50 | 0 fault |
| 8 | 20:16 | 314 | 346 | in-world HP 88 | 0 fault |
| 9 | 20:21 | 315 | 351 | CONTINUE? | YES at 20:22:58 → HP 110 |
| 10 | 20:26 | 316 | 356 | in-world HP 1 | living save after YES |
| 11 | 20:31 | 317 | 361 | in-world | 0 fault |
| 12 | 20:36 | 318 | 366 | in-world | last complete cycle |

Final sslog: **save_slot 318**, **resume 366**, **fault 0**. One `exit:`
(mixer `ExitProcess` park tid 500 at load 8). Mixer stayed parked.
Held heaps UNCHANGED on every copy. Wrapper heap `0C010000` grew to
**607936 KB (~594 MB)** by save 318. Slot ~523 MB, 173 regions, 49
threads, mapid 1.

## Host freeze — sitting ended

Cycle 12 finished 20:37:33 (`idle 263s until next save`). Cycle 13
never started. Previous-boot journal from 20:40:51–20:41:55:
`systemd-journald: Under memory pressure, flushing caches`; PipeWire
ALSA `Broken pipe`; Xorg `event processing lagging … system is too
slow`. Catt Man found the laptop unresponsive and rebooted **20:42**.

This box is 7.1 Gi RAM. Same-boot OOM already killed
`pipewire-pulse` at **18:35** when linux **118091** / Steam snap died
after 845 loads. Wrapper-heap growth (held, not rewound) plus GPU
payload plus the 523 MB slot is eating the machine. Not a `fault:`,
not a CONTINUE overlay, not a KEY miss.

After reboot: **208448 gone**, **247198 gone**, Steam not running,
sslog survived on disk. Archived
`logs/sslog-cadence-save5-208448.txt`. Did **not** relaunch — another
GPU=3 TEX_SCALE=1 stretch on this RAM budget will likely lock the
UI again.

## Does a new save at 5 min disrupt session state?

**No, through 12 cycles.** New KEY_1 copies live state (in-world or
CONTINUE). Five KEY_2s restore that snapshot. Held-hash verdict stays
UNCHANGED. What killed the *box* is memory pressure from the growing
wrapper heap, not a bad restore.

No dump of `rabiribi.exe`. USER32 not rewound. `xrestore.ps1` not run.
