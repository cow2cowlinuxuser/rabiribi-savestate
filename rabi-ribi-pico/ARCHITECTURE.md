# Architecture

## One sentence

A native 32-bit process whose OS is a library we link into it, whose GPU is real
but command-interposed, and whose entire reachable host surface equals a fixed
ABI — so capture is a fork and escape is unsatisfiable.

## The three properties, and where each comes from

| Property | Mechanism | Cost we accept |
|---|---|---|
| **Native speed** | Game runs on the bare CPU; no CPU virtualization. | Boundary completeness is *our* responsibility, not a hypervisor's. |
| **Real, fast GPU that still rewinds** | Interpose the *command* boundary (already done in the `*_sw` shims); back it with a real device; never snapshot the GPU — snapshot the replayable *description*. | Restore replays resource creation into a fresh device (a few ms), rather than restoring device bytes. |
| **Death boundary** | We own the loader + `GetProcAddress`, so the reachable syscall surface = the wired ABI. Unwired calls resolve to nothing. | Every real edge must be enumerated once, up front. |

## Layering

```
  +-------------------------------------------------------------+
  |  rabiribi.exe   (native x86, runs on the real CPU at speed)  |
  |    imports resolve ONLY to the pico layer, never to the host |
  +----------------------------+--------------------------------+
                               |  (import table + GetProcAddress shim
                               |   are the entire reachable surface)
  +----------------------------v--------------------------------+
  |  PICO LIBRARY OS  (in-process, ours)                        |
  |                                                             |
  |  device world .......... d3d9_sw / d3d11_sw / gl_sw /       |
  |                          xa2_sw / ds_sw / xinput_sw         |
  |  clock ................. hooked QPC (rewindable)            |
  |  memory authority ...... gameheap + arena + ledger          |
  |  sync .................. our wait/wake (own the primitives) |
  |  virtual filesystem .... game dir (ro) + save area (rw)     |
  |  rewind engine ......... savestate.c  (freeze/copy/restore) |
  |                                                             |
  |  DOWN-CALLS (the only real host edges) ------------------+  |
  +----------------------------+----------------------------|--+
                               |                            |
  +----------------------------v----------------------------v--+
  |  HOST (real Windows)                                        |
  |    - real virtual memory (NtAllocateVirtualMemory family)   |
  |    - real threads on the NT scheduler                       |
  |    - ONE window + present surface                           |
  |    - ONE real GPU device (ours, not the game's)             |
  |    - a controlled set of real files (backs the VFS)         |
  |    - a Job object that owns lifetime and kills on close     |
  +-------------------------------------------------------------+
```

Everything *not* on the down-call list — process creation, registry, network,
COM activation, shell, arbitrary module loading — has no wire to the host. That
is the death boundary, drawn as a data-flow fact rather than a permission.

## Why the boundary is enforceable in user mode

We do not hook `ntdll` in place — that was the layer the earlier work found
uninterceptable, and it is the wrong layer anyway. Instead we **own the loader**:
map `rabiribi.exe` ourselves and bind its imports to the pico layer from the
first instruction, so the game never holds a pointer to a real subsystem export.
Two consequences:

1. **The static surface is knowable before the game runs.** [pe_imports.ps1](../pe_imports.ps1)
   already lists the import table; closing that set to the ABI is a finite,
   checkable task, not a game of whack-a-mole at crash time.
2. **The dynamic surface is ours too.** Every `GetProcAddress` / `LoadLibrary`
   goes through our shim, which returns addresses only for sanctioned names.
   There is no path from inside the process to an unsanctioned host export,
   because the only name resolver the game can reach is ours.

If we later want the boundary enforced at the kernel edge instead of by import
redirection — so that even a hand-rolled syscall stub the game synthesizes lands
on us — that is the WSL1 mechanism: a **Pico provider** (`PsRegisterPicoProvider`)
in a driver. That is the heavy option in [ROADMAP.md](ROADMAP.md), Phase 4; the
user-mode loader is enough for everything short of a game that builds its own
`SYSCALL` instructions, which this one does not.

## The GPU resolution, concretely

This is the property that beats a VM, so it gets its own section.

The `*_sw` backends are already **command interceptors with full shadow state**.
Look at `d3d9_sw.c`: it tracks every `SwTexture`, `SwVB`, `SwIB`, the render-state
array, the vertex/pixel constant banks, the declarations, the samplers — the whole
logical device. Today that shadow drives `swrast` (CPU). The shadow *is* the
replayable description of the GPU, and it lives in rewound memory.

The pico step is to add an **alternate backend** behind that same shadow:

- **During play:** forward draws to a real D3D device that the *pico* owns
  (one down-call channel). Native GPU speed. No cycles lost to a VM.
- **At capture:** snapshot nothing on the GPU. The description is already in
  rewound memory.
- **At restore:** replay resource creation from the shadow into a *fresh* real
  device. This never wedges the driver, because we hand it a clean rebuild from
  the game's own memory — not stale kernel/device state. Same "re-derive, not
  restore" rule `xa2_sw` already uses for `waveOut` (the device is rebuilt from
  nothing; only the voice *state* rewinds).

So the GPU is real and fast, the pico owns the device (a single, freezable edge),
and rewind works because only the *description* rewinds. A VM cannot do this; the
interposition layer can.

For pass 1 the rebuild is taken further still: contents are not carried at all,
only identity, and every resource comes back as a shader-synthesized stand-in
that is structurally valid but not necessarily correct. The invariant drops from
"restore the picture" to "every handle resolves to *something*," which self-heals
as the game re-renders. See [VALVES.md](VALVES.md) for the full cut-list and the
one exception (content the game reads back into its logic).

## Where the rewind edge goes in this model

The soft-freeze partition problem (`savestate_exclude`, the `DFD777C1`-class
cross-line-pointer bugs) does not vanish, but it **shrinks to the ABI**. The set
of "present-tense, foreign-owned" state stops being something discovered one
crash at a time and becomes the down-call list itself — known up front, small,
and the same list that defines the death boundary. One list, two purposes:

- read as *data flow*, it is the reachable host surface (security: the death boundary);
- read as *state ownership*, it is the exclude/reconstruct set (correctness: the rewind partition).

That collapse — one enumerable boundary serving both escape-prevention and
capture-consistency — is the whole reason the picoprocess is the right shape.
