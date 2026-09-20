---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# Unnamed heaps held: 001DCA88 warning gone, winevulkan+23434 still left

Iterate-until-die after [bound-save10-presenter-idle.md](bound-save10-presenter-idle.md). Goal remains 5 runs of 1 save + 10 restores, GPU=3 TEX_SCALE=1, one process, simple movement.

Heap-bound DLL stayed in the snap game dir. `steam://rungameid/400910` only. CONTINUE beach. No cross-session. USER32 not rewound. Exe not dumped. `xrestore.ps1` not run.

## Door (35034)

Idle handshake cannot ack: `request()` owns the Present thread. Flush-then-rewind is closed from both threads (helper 20929/24023, presenter 35034). Do not park winevulkan (26458 left).

Next door was: close CBs without putting DXVK into unix calls across freeze, **or** stop rewinding what `vkEndCommandBuffer` still holds (`001DCA88` on unnamed heap `001D0000`).

Hypothesis: ALLHEAPS=2 rewinds no-clear-owner Wine heaps because Wine has no `0xFFEEFFEE` HEAP_SEGMENT (0 votes). linux **82152** (TEX_SCALE=2) lived three pairs when those heaps stayed in the present. Hold them on this branch.

## Death: linux **37661**, unnamed heaps held

DLL md5 `23b05ff1a3bbc6d677b7cc14b42cad56` (`82fd84a`). CONTINUE beach. KEY_1 lived: mapid 1, **175** regions (was 178), 521.7 MB, 48 threads. Compact 1 header / 0.00 MB.

Heaps at the save:

```
heap 00150000 the process heap         left in the present (0 vote(s))
heap 0C210000 the game's runtime       rewound (0 vote(s))
heap 0BFE0000 this wrapper's own       left in the present (0 vote(s))
heap 02070000 no clear owner           left in the present (0 vote(s))
heap 001D0000 no clear owner           left in the present (0 vote(s))
```

No `system thread 524 holds 001DCA88 ... which we rewind`. The unnamed-heap hold did that job.

First KEY_2 **lived**: `resume: done`, player back x=25958 y=8149, 169 restored. Then immediately:

```
exitpath: RtlExitUserProcess called from 7BE9FBEA (kernel32.dll+FBEA)
  frame 0: ebp 0C99FBDC, returns to 77EA3434 in winevulkan.dll+23434
exitpath: adapter RtlExitUserProcess - not parking the presenter
```

Process gone during restore 2. Present was not frozen. Abort RVA moved: 26458/31417/33277 were `winevulkan.dll+18E57`; this sitting is `+23434`.

Still:

```
PARTITION LEAK: 2 region(s) / 1.1 MB are inside a heap we leave in the present but were captured anyway, first at 00150000 in the process heap. The restore will rewind part of that heap
```

Exitpath names the remaining rewind:

```
saved [ebp-C]=0015D390 committed, in the the process heap heap at 00150000, which we leave in the present, restored from the save, first word 01CDC0DE
saved [ebp-10]=0C4E7AC0 committed, restored from the save, first word 00000010
```

DXVK (`d3d11.dll+24FC6F`, not the wrapper at `60000000`) called winevulkan with a process-heap object the snapshot had put back. Holding `001D0000` closed the handle-page warning; Wine still has no segments, so the **process-heap and wrapper-heap handle pages** stayed in the slot (0.06 MB + 1.06 MB = 1.1 MB).

TEX_SCALE=1 still dies. 82152 TEX_SCALE=2 lived because that sitting excluded handle pages of **every** held Wine heap, not only unnamed.

## Protocol

1 save + 1 restore of the 1+10×5. Mutation reload-then-save not reached. Wrapper heap not measured across tens of pairs. Not cross-session. USER32 not rewound. Exe not dumped. Game left gone.

MemAvailable stayed ~3.9 Gi. No UI lock from memory pressure.

## Next lever

Do not Flush. Do not wait for Present after `request()`. Do not park winevulkan. Do not rewind `001DCA88` (closed: warning gone).

**Stop capturing the handle page of every Wine heap we leave in the present** — process heap `00150000` and wrapper heap included — so `0015D390` is not restored under winevulkan. Not the header-list walk (process-heap growth); that killed load 1 in `rabiribi.exe`.

Close CBs without DXVK unix across freeze remains open if the abort survives a consistent partition.

## Left on disk

`Downloads/frierenserver-gpu-insession/` logs `sslog-bound-save10-unnamed.txt` / `d3d11-bound-save10-unnamed.log` / `bound-save10-unnamed.out`.
Laptop clone commit `82fd84a` on `cursor/wrapper-heap-bound-bd2d` (no GitHub push from this box).
Next sitting: [bound-save10-handlepage.md](bound-save10-handlepage.md).
