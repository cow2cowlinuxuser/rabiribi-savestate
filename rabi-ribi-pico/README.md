# rabi-ribi-pico

A death-bounded picoprocess host for one game.

## What this is

Not a sandbox. Not a VM. A **substrate**: the game runs natively on the bare
CPU with a real GPU, but every edge it has to the operating system terminates
inside a layer we own. We are not walling the game off from the host — we are
*being* the host it runs on.

The organizing principle is the **death boundary**, and it is stronger than a
sandbox because it is not a policy that denies calls — it is an absence:

> A sandbox blocks dangerous calls that exist. Here, the dangerous calls were
> never wired up. `CreateProcess`, `socket`, `RegOpenKey`, `ShellExecute`,
> `LoadLibrary` of anything outside the sanctioned set — these do not resolve to
> a guarded host call, they resolve to nothing, or to a stub that ends the world.
> An escape attempt, whether a privilege-escalation payload or just an unattended
> behaviour we did not want, is not *forbidden*; it is *unsatisfiable*. The call
> it would need was never connected to a real subsystem in the first place.

The reachable OS surface of the process is exactly the set we choose to wire.
Everything else is a hole.

## Why not a VM

A VM gives a closed, capturable boundary — which we want — but pays for it with
two costs we refuse:

- **CPU tax.** Trap-and-emulate on every privileged action; cycles lost to
  VMEXITs even when nothing needs emulating.
- **The GPU wall.** A VM forces a choice between an *emulated* GPU (software,
  slow — back to `swrast`) and *passthrough* (VFIO/SR-IOV: real and fast, but a
  passed-through physical GPU **cannot be snapshotted** — the same failure as the
  early "it took my GPU out" experiments). No VM is both accelerated and
  checkpointable.

The picoprocess virtualizes *neither* the CPU nor the GPU. It interposes only the
**boundary**. The CPU stays bare-metal; the GPU stays real and fast; only the
narrow host ABI is ours to freeze and reconstruct. See [ARCHITECTURE.md](ARCHITECTURE.md).

## Prior art (so we know the model is real, not invented here)

- **Drawbridge picoprocess** (Microsoft Research) — a process whose host
  interaction is restricted to a tiny fixed ABI, with a library OS above it.
- **WSL1's pico provider** (`PsRegisterPicoProvider`) — Linux binaries running
  *natively on the NT scheduler*, no VM, zero CPU-virtualization tax, through a
  thin syscall-translation layer. WSL2 became a VM only because it needed perfect
  fidelity for *arbitrary* binaries. We run **one** game whose syscall set we have
  already been censusing — which is the entire reason this is buildable when a
  general VM-equivalent is not.
- **Gramine / Graphene** — a library OS in a process, same shape.

## The files that already exist

The device boundary is already ours. This project's job is to close the *rest* of
the boundary and put a lifecycle cage around it. See [INVENTORY.md](INVENTORY.md)
for what covers what, and [ABI.md](ABI.md) for the boundary spec.

## Documents

- [ARCHITECTURE.md](ARCHITECTURE.md) — the model, the death boundary, the GPU resolution.
- [ABI.md](ABI.md) — the wired down-call set, and the unwired (death) set.
- [VALVES.md](VALVES.md) — the one-way valves: what survives, what rebuilds, what is synthesized on restore.
- [INVENTORY.md](INVENTORY.md) — existing shims mapped onto the boundary; the gaps.
- [ROADMAP.md](ROADMAP.md) — phases from what exists to a closed boundary.
