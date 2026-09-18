# frierenserver EXCLSKIP sitting

PARTHOLD's same-boot A→B refuse was `load: refused, 2 of 158 regions unrestorable` — excluded-now stacks/TEBs, not belongs-elsewhere.

`D3D9SW_EXCLSKIP=1` (on by default in this tree) counts those as `skipped_excl` and lets the restore continue instead of folding them into the unrestorable veto. Later sittings with EXCLSKIP on logged the skip line:

```
  N region(s) skipped because they are excluded now - live stacks or TEBs …
```

and then either copied or hit the older belongs-elsewhere veto.

The write loop has to skip those regions too. A pre-check that logs "skipped" and then `D3D9SW_DIFFWRITE` memcmp's the same range is how Proton previously faulted at `d3d11.dll+50680` on a FREE page inside a live stack. That write-loop skip is part of the belongs-elsewhere coalesce commit on this branch.

EXCLSKIP does not invent a new class. It stops refusing over a region we cannot write without stomping a running thread.
