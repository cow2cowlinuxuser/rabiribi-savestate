# Inventory: what covers the boundary today, and the gaps

Honest accounting of the existing tree against the [ABI.md](ABI.md) boundary.
The verdict up front: **the device world is nearly done; the host boundary is
wide open; there is no loader and no cage yet.** The pico project is mostly the
second and third of those.

## How interposition works today

Passive **DLL search-order substitution**, not a loader. The `.def` files export
the real API names (`d3d9.def` → `Direct3DCreate9`, `xaudio2_9.def` →
`XAudio2Create`, `xinput.def` → `XInputGetState`, …) and ship a same-folder DLL
that wins the search over the system copy. `tramp.c` trampolines vtables for
tracing; `chain.c` is offline image analysis. **There is no custom loader and no
manual mapping** — which is exactly why the boundary below the device APIs is not
closed today: `kernel32`/`ntdll`/`advapi32`/`ws2_32` calls go straight to the
real host, unseen.

## WIRED coverage

| ABI edge | Covered by | State |
|---|---|---|
| Graphics D3D9 | `d3d9_sw.c` + `swrast.c` + `vsinterp.c` | **Strong** — full shadow state; software backend. Needs real-GPU backend added behind the shadow. |
| Graphics D3D11/DXGI | `d3d11_sw.c` + `dxgi_fwd.c` + `bcdec.h` + `dxbc.c` | **Strong** — Unity/DxLib path; arena + ledger for VA discipline. Same real-GPU-backend TODO. |
| Graphics GL | `gl_sw.c` + `gl_stubs.c` | Present; lower priority for this title. |
| Audio XAudio2 | `xa2_sw.c` + `xa2_fwd.c` | **Strong** — pushed model, rewindable clock, own mixer that parks, `waveOut`-only sink, callbacks on the game's thread. Reference design for "own the party." |
| Audio DirectSound | `ds_sw.c` + `dsoundhook.c` + `dsound_fwd.c` | Present; XAudio2 is the live path for rabi-ribi. |
| Input (pad) | `xinput_sw.c` | Present. |
| Input (kbd/mouse) | `savestate_key_edge/held` in `savestate.c` | Partial — reliable edges exist; not yet a full fed-input source. |
| Clock | hooked QPC (used by `xa2_sw.c::now_qpc`) | **Strong** — rewindable, single source of time. |
| Memory authority | `gameheap.c` + arena + ledger (`d3d11_sw.c`) | **Strong** — models the monotonic-VA-vs-rewound-contents problem the ledger was built for. |
| Rewind engine | `savestate.c` + `savestate.h` | **Strong** — freeze/copy/restore, exclude list, census, soak, thread-set diff, fault-side naming (`savestate_rewinds`). |
| Present / window | present path in each `*_sw` | Strong for present; window is created by the game today, not by the pico. |

## WIRED — not yet done

- **Real-GPU backend behind the `*_sw` shadow.** Today the shadow drives
  `swrast` (CPU). Add an alternate backend that submits to a real device the pico
  owns, keeping the shadow as the replayable description. This is the "accelerated
  *and* checkpointable" payoff. (ROADMAP Phase 3.)
- **Fed keyboard/mouse source.** Promote the existing edge tracker into a full
  input provider so the game polls *us*, never the real device.
- **Pico-owned window.** The window should be created and owned by the pico, so
  it is one clean present-tense object, not something the game spins up behind us.

## UNWIRED — the gaps that are currently OPEN to the host

These are the death-boundary holes that exist *right now* because there is no
loader mediating `kernel32`/`ntdll`. Each is a real edge the game can reach today:

| Surface | Today | Needed |
|---|---|---|
| Process creation | passes to host | stub → fail/terminate |
| Networking (`ws2_32`, wininet, winhttp) | passes to host | stub → fail |
| Registry (`advapi32`) | passes to host | redirect to VFS/config |
| `LoadLibrary`/`GetProcAddress` | passes to host | mediated allow-list |
| File I/O paths | passes to host, arbitrary paths | VFS namespace (game dir ro + save area rw) |
| Token/privilege APIs | passes to host | stub → no-op + log |
| Environment / persistence | passes to host | frozen/config-backed |

## Missing scaffolding (net-new for this project)

- **The loader** (`pico_loader`): map `rabiribi.exe`, bind imports to the pico,
  own `GetProcAddress`. This is what turns "device APIs are ours" into "the whole
  reachable surface is ours." Nothing today does this.
- **The lifecycle cage** (`pico_job`): the `KILL_ON_JOB_CLOSE` Job object, no
  breakaway, active-process-limit 1.
- **The VFS** (`pico_vfs`): the file namespace behind the file ABI.
- **The boundary auditor** (`pico_audit`): consumes `pe_imports.ps1` + a runtime
  `GetProcAddress` log and asserts every name is WIRED or UNWIRED — the tool that
  lets us *claim* the boundary is closed rather than hope it.
