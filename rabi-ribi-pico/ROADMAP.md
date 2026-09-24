# Roadmap

Ordered so that each phase produces something testable, and so the earliest
phases buy the most boundary for the least new code. The device world already
works; we build inward (close the boundary) before outward (accelerate).

## Phase 0 — Measure the surface (no new runtime code)

Turn "we think the boundary is closed" into a checklist.

- Run [../pe_imports.ps1](../pe_imports.ps1) against `rabiribi.exe` and every DLL
  it loads. Freeze the result as `surface_static.txt`.
- Add a `GetProcAddress`/`LoadLibrary` logger to the existing shims and capture a
  full play session → `surface_dynamic.txt`.
- Classify every name into WIRED / UNWIRED per [ABI.md](ABI.md). The residue —
  names in neither — is the exact work list for Phase 2.
- **Readback check (validates the GPU valve early).** From the existing
  intercepted-call logs, grep for any GPU→CPU readback the game actually uses —
  `GetRenderTargetData`, `GetFrontBufferData`, staging-surface `Map`/`Lock` reads,
  occlusion/`GetData` queries. Record the finding in `surface.md`. If the game
  never reads GPU content back into its own logic, the [VALVES.md](VALVES.md)
  synthesize-any-handle rule is proven safe blanket-wide *before* Phase 3 builds on
  it; if it does, that specific resource is marked carry-for-real, not synthesize.
- **Exit criterion:** a single `surface.md` where every symbol has a verdict and
  the readback question is answered yes/no with the evidence.

## Phase 1 — The cage (cheap, independent, immediate value)

Lifetime control that does not depend on the loader.

- `pico_job`: launch the game already inside a Job with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, breakaway disabled, active-process
  limit 1.
- **Exit criterion:** killing the host launcher provably tears down the whole
  world; the game cannot spawn a surviving child.

## Phase 2 — Own the loader, close the boundary

The heart of the death boundary.

- `pico_loader`: map `rabiribi.exe`, resolve its imports to the pico layer,
  install the `GetProcAddress`/`LoadLibrary` allow-list shim. Keep the current
  same-folder device DLLs as the backends the loader binds to.
- Wire the UNWIRED stubs (process/net/registry/token → fail or terminate;
  registry → config; file paths → VFS via `pico_vfs`).
- `pico_audit`: assert at startup that no import resolves outside WIRED∪UNWIRED;
  refuse to run (or loudly log) on a leak.
- **Exit criterion:** the game runs to a playable state with `surface.md` fully
  green, and a deliberate escape probe (a call to `CreateProcess`, `socket`,
  `RegOpenKey`) lands in a stub, not the host.

## Phase 3 — Real GPU behind the shadow (the acceleration payoff)

Only now, once the boundary is closed and rewind is stable on the software path.

- Add a real-device backend behind the `d3d9_sw` / `d3d11_sw` shadow (see
  ARCHITECTURE "GPU resolution"). During play, draws forward to a pico-owned real
  device; the shadow remains the source of truth.
- Restore path: per [VALVES.md](VALVES.md), a fresh device, shaders recompiled
  faithfully, every other live handle synthesized as a structurally valid
  stand-in. Contents are not carried. Verify it never wedges the driver (the
  failure the whole project started from).
- **Decide the stand-in shape source — the descriptor/lazy fork:**
  - *descriptor-carried* — keep each resource's `w/h/fmt` header (already in the
    `SwTexture` shadow), drop `pixels`; stand-ins match dimensions. A hair safer at
    size-sensitive binds.
  - *fully-lazy / fault-in* — carry only identity; materialize a 1×1 default on
    first touch, inflating when a declared size appears. Smallest carry.
  Pick one here; it is the last open architectural choice in the GPU valve, and
  the readback finding from Phase 0 tells you whether any resource must opt out of
  synthesis regardless.
- **Exit criterion:** native-GPU frame rates *and* a surviving restore, on the
  same build, back to back.

## Phase 4 — (Optional) kernel-edge enforcement

Only if a future need arises for a boundary that survives the game synthesizing
its own syscalls (rabi-ribi does not).

- A Pico provider (`PsRegisterPicoProvider`) makes the pico the syscall dispatch
  target at the kernel edge, the way WSL1 did — so even a hand-rolled `SYSCALL`
  lands on us. Heaviest option; carries a driver and a BSOD risk.
- **Exit criterion:** a synthetic syscall stub in a test binary is intercepted,
  not executed by the host.

---

## Sequencing rationale

- **Cage before loader:** lifetime control is independent and cheap, and it makes
  every later experiment safe to abort.
- **Loader before GPU:** a closed boundary shrinks the rewind partition to the ABI
  (ARCHITECTURE, last section). Debugging the real-GPU restore against a *known*
  boundary is tractable; against an open one it is another `DFD777C1` hunt.
- **Software GPU stays the reference backend forever.** The real-device backend is
  an accelerator bolted onto a shadow that must remain correct on its own, so a
  GPU-restore regression can always be bisected against the software path.

## First concrete step

Phase 0's `surface.md`. It is the artifact that tells us how far the boundary
already is from closed, and it is pure measurement — no risk, and it sizes
everything after it. It also directly answers the open question from the design
chats: *which of the game's edges are not yet ours.*
