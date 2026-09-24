# The boundary spec

The pico ABI is the complete list of real host edges. Two rules:

1. **If it is on the WIRED list, it is the *only* way the game can reach that
   capability, and the pico owns the real object.**
2. **If it is not on the WIRED list, it is UNWIRED: the call resolves to a stub
   that fails or terminates. Not blocked — unconnected.**

The union of these two lists must cover every import in
[../pe_imports.ps1](../pe_imports.ps1) output plus every name the game asks for
via `GetProcAddress`. Anything in neither list at first run is a **boundary leak**
and must be triaged into one of them before the boundary is claimed closed.

---

## WIRED — the down-call set (the real edges)

These cannot be serviced purely inside the process because they touch real
hardware or the real kernel. Each is a single, auditable channel.

### Memory (the one real allocator)
- Reserve / commit / decommit / release virtual address space.
- Change protection.
- **Owner:** `gameheap.c` + the arena + the ledger already model this. The ledger
  exists precisely because *address space is monotonic while contents rewind* —
  keep it as the memory authority of record.
- **Rewind role:** committed game memory rewinds; the ledger/arena tables are
  excluded (present tense) so they describe reality after a restore.

### Threads & scheduling (real threads, our table)
- Create thread, exit thread, get/set context, suspend/resume, TLS get/set.
- **Owner:** new `pico_thread`. Threads run on the real NT scheduler (native
  speed); the pico keeps the authoritative thread table so a restore knows the
  saved thread set (`savestate_thread_set` already reports the mismatch).
- **Death note:** thread creation is wired; *process* creation is not (see UNWIRED).

### Synchronization (own the primitives)
- Wait-on-address / wake, events, mutexes, critical sections, semaphores.
- **Preference:** service *internally* where possible, so the game cannot mint a
  real cross-process kernel object. A purely in-process wait primitive is also
  trivially freezable — no kernel handle to reconstruct on restore.

### Clock (virtual, rewindable)
- `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `GetTickCount(64)`,
  timers, `timeGetTime`.
- **Owner:** the hooked QPC already used by `xa2_sw.c` (`now_qpc` reads the
  rewound counter on purpose). This is the clock the whole world reads.
- **Rewind role:** the clock rewinds with the game; that is what keeps the audio
  sample count and the game's sense of time agreeing after a restore.

### Virtual filesystem (no arbitrary paths)
- open / read / write / seek / close / stat / enumerate.
- **Backing:** the game's install directory mapped **read-only**, plus a single
  **read-write save area** we control. Nothing else is nameable.
- **Death note:** a path outside the VFS root does not fail a permission check —
  it does not exist in the namespace the game can address.
- **Rewind role:** file *positions* are pico state and rewind; the RW save area is
  present tense (survives, like a real disk would).

### Display / present (one window, one surface)
- Create the single game window; present a finished frame to it.
- **Owner:** the present path in `d3d9_sw.c` (`swrast_present`, GDI `StretchDIBits`)
  and its D3D11/GL equivalents. The window is the one host UI object we cannot
  avoid; keep it minimal and singular.
- **Rewind role:** the window handle is present tense (excluded); the framebuffer
  contents are rebuilt from the next frame, not restored.

### Input (fed in, never polled from real devices)
- Keyboard, mouse, gamepad state delivered *to* the game by the pico.
- **Owner:** `xinput_sw.c` for pads; a keyboard/mouse shim for the rest. The game
  can only see input the pico chooses to present (also what makes unattended
  soak/replay possible — `savestate_key_edge` already exists for reliable edges).

### Graphics command submission (the accelerated backend)
- Submit the shadowed command/resource set to the real GPU device the pico owns.
- **Owner:** a new real-device backend behind the existing `d3d9_sw` / `d3d11_sw`
  shadow (see ARCHITECTURE "GPU resolution"). The device is the pico's, never the
  game's; the game only ever sees our software-shaped interface.
- **Rewind role:** the device is present tense and rebuilt on restore; the shadow
  (resource set + state) rewinds and is replayed into the fresh device.

---

## UNWIRED — the death set (no host edge exists)

Each of these resolves to a stub that returns failure, or terminates the world if
the attempt is itself the signal we want to act on. **None is connected to a real
subsystem.** This list is a starting set, not exhaustive; the boundary-closure
task (ROADMAP Phase 2) is to prove every non-WIRED import lands here.

- **Process / image spawning:** `CreateProcess*`, `ShellExecute*`, `WinExec`,
  `system`, `CreateRemoteThread` into anything but self.
- **Networking:** `ws2_32` (`socket`, `connect`, `send`, `recv`, `WSAStartup`),
  `wininet`, `winhttp`, `urlmon`. A game that never phones home cannot be made to.
- **Registry:** `RegOpenKey*`, `RegSetValue*`, `RegCreateKey*`. If the game reads
  a setting from the registry, that read is redirected to the VFS/config, not the
  real hive.
- **Arbitrary module loading:** `LoadLibrary*` / `GetProcAddress` for any name
  outside the sanctioned set (the WIRED shims and the game's own known DLLs). This
  is the linchpin — it is what makes the *dynamic* surface as closed as the static
  one.
- **Persistence / config surfaces:** scheduled tasks, services, startup keys,
  WMI, environment mutation that outlives the process.
- **Privilege / token surfaces:** `AdjustTokenPrivileges`, `OpenProcessToken`
  writes, `SetSecurityInfo`, anything that would change what the process *is*
  allowed to do. A privilege-escalation attempt calls into a stub; the primitive
  it needs was never wired to the real API, so escalation is a no-op that we log
  (and may treat as a terminate signal).
- **Other processes / global objects:** `OpenProcess` of anything but self,
  named kernel objects that could be shared out of the world, clipboard,
  global hooks, window messages to foreign windows.

---

## The lifecycle cage (how the world dies cleanly)

Independent of the ABI, the whole process tree lives inside a **Job object**:

- `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` — closing the job handle kills everything
  in it, atomically. That is the "it can die" the design asks for.
- Breakaway disabled — nothing can escape the job into a sibling process.
- Optional active-process limit of 1 — since process creation is UNWIRED anyway,
  this is a belt-and-braces assertion that the count never exceeds one.

The Job is the *lifetime* boundary; the ABI is the *reach* boundary. Together:
the world can live, can die on command, and can do nothing outside itself —
because the calls to do so were never wired, and the one handle that owns its
life is ours.
