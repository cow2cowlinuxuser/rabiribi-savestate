# Windows savestate ownership
## A debate-settler for in-process save and restore

**Status:** synthesis of measured facts from this repository and the 2026-09 Rabi-Ribi workstream.  
**Audience:** anyone arguing about why a byte-perfect restore still dies, what “owning” memory means under Windows, or what to build next.  
**Rule inherited from `restore_invariants.md`:** mechanisms that were only theorised and then measured absent are recorded as such; estimates are marked.

Related docs (do not duplicate their detail — this file is the map):

| Doc | Role |
|---|---|
| `restore_invariants.md` (repo root) | Class A/B/C + post-restore checklist |
| `docs/rabiribi_static_crt.md` | Allocator surface + false ucrtbase attribution |
| `crash_histogram.md` (repo root) | 100-run death shape (harness) |
| `rewind_state_table.md` (repo root) | Future multi-state / undo design (mostly unbuilt) |
| `README.md` | Current engine claim and known gaps |

---

## 0. One-sentence definition

**Ownership**, for this engine, is not “which DLL we authored” and not “which API we hooked.”

> **You own a thing iff, after restore, no agent outside the rewound set can mutate it or chase a pointer into it — and you do not chase pointers into anything that agent still owns in the present.**

Everything else in this document is that sentence expanded into Windows reality and into what *this* codebase actually does.

---

## 1. The atom: save → hold → restore → resume

An “atomic” savestate is not one CPU instruction. It is a **protocol** with a begin, a frozen middle, and an end. Failures almost never happen in the copy; they happen because the protocol’s claim about who was frozen was false.

### 1.1 Intended timeline (what we claim)

```
 request save
    │
    ▼
┌─ SUSPEND ─────────────────────────────────────────────┐
│  Stop (some) threads. Claim: nobody mutates the set. │
├─ SETTLE (optional) ───────────────────────────────────┤
│  Wait until hot writers leave critical modules.        │
├─ PARTITION ───────────────────────────────────────────┤
│  Decide rewound / present / excluded / contested.      │
├─ CAPTURE ─────────────────────────────────────────────┤
│  Copy regions, heap blocks, contexts, TLS, clocks,     │
│  audio cursors into a store (memory today).            │
└─ RESUME (save complete) ──────────────────────────────┘

 … game runs …

 request load
    │
    ▼
┌─ SUSPEND ─────────────────────────────────────────────┐
├─ RESTORE ─────────────────────────────────────────────┤
│  Write bytes back. Fixup peers (audio seek, fntab…).  │
│  Run invariants (heap validate, thread-set, …).        │
└─ RESUME (load complete) ──────────────────────────────┘
    │
    ▼
  first instructions after resume  ← deaths cluster here
```

### 1.2 Measured costs that matter (order-of-magnitude)

From `rewind_state_table.md` (2026-08-30 logs; title may differ from pure Rabi-Ribi):

| Phase | Measured |
|---|---|
| Suspend ~55 threads | ~30 ms |
| **Exclusions / ownership reachability** | **~340 ms** (dominates) |
| Walk | 15–25 ms |
| Section + copy (~481 MB class) | 160–200 ms |

**Copying is not the hard problem. Deciding what is ours is.**

### 1.3 What “complete” means in the README’s sense

A save/restore is **copy-complete** when:

- every captured region writes back byte-for-byte (`0 WRONG`);
- no captured region is skipped;
- tracked DirectSound buffers are stopped and seeked (and similarly for any audio peer you treat as in-set).

The engine often reaches copy-complete and **still** faults within a frame of resume. That is not a paradox under the ownership definition: the copy can be perfect while the **partition** is wrong, or while a **peer outside the set** still holds a pointer into the set.

---

## 2. Three places a byte can live

Every address in the process falls into exactly one of these at restore time. Debates that skip this taxonomy talk past each other.

### 2.1 Rewound (in the snapshot, written back)

Typically: selected `MEM_PRIVATE` / `MEM_IMAGE` regions, selected heap blocks on “game” heaps, stacks/contexts/TLS of **rewound threads**, clocks you chose to rewind.

After restore, these bytes claim to be the past.

### 2.2 Present (never captured, or deliberately left alone)

Typically: ntdll/ucrtbase/kernel images you do not rewrite; Windows heaps you marked not-ours; **system / transient thread stacks and TEBs**; Steam/overlay threads; your own excluded “held” control memory; anything `MEM_MAPPED` your filter skips (~4.7 MB measured gap).

After restore, these bytes continued aging during the save window and beyond.

#### 2.2.1 The engine is a peer of the island it creates

Held memory is **not rewound**, and not-rewound is the definition of a peer. Being ours does not exempt it. Two confirmed Rabi-Ribi deaths, both root-caused and fixed, had this engine as the Class B counterparty:

| fault | mechanism |
|---|---|
| `swrast`, read at address 4 | A `static __thread int` in a held module. Being held keeps our *statics* out of the rewind, but thread-local storage is not ours — the loader allocates the per-thread block and the array that indexes it on a heap we **do** rewind. The restore left the slot reading zero; the next worker read through null. |
| `d3d11.dll+42012` = `cb_flush+0x92`, 0 frames after restore | The deferred-callback ring is a static in a held module holding `XA2Callback *` into game memory. Present-tense container, past-tense contents: after a rewind the pointer addressed bytes belonging to an object that did not exist at save time, and we read the first word as a vtable (`F5C8BE54`). |

**Rule.** Any held structure holding a pointer into rewound memory must be emptied at the park. Any held code relying on loader-allocated per-thread storage must keep that state in its own data instead. Both fixes are in the tree; neither was visible from the Windows-as-the-shore framing, because in both cases the shore was us.

### 2.3 Store (the snapshot cargo)

Today: in-process buffers (often near “held” / engine memory).  
Proposed: raw scratch-pad file (see §8).

The store must be **present-only by construction**. If restored game state can reach into the store, you have invented a new Class A/B against yourself.

---

## 3. The only three ways restore can be inconsistent

From `restore_invariants.md` — closed list. Mono/Unity/Rabi-Ribi only choose *which structures* fall into which class.

### Class A — outward straddle

Something **inside** the snapshot refers to something **outside**.  
Restore puts the reference back; the target has moved on.

**Confirmed:** growable unwind tables (`RtlpDynamicFunctionTable`); COM pointer into engine memory (`ID3D11Query`) — fixed by record/replay and object retirement.

### Class B — inward straddle

Something **outside** the snapshot refers to something **inside**.  
The reference stays present-valued; the target is rewound underneath it.

**Confirmed shape:** system-thread TEBs holding ~8 game-heap blocks (direct-hit veto built; closure veto measured harmful 3317→7483 system-owned).  
**Structural twin:** any live peer thread you did not rewind (Steam, threadpool, audio engine) holding game pointers.

### Class C — mid-mutation capture

Both halves are inside the snapshot; the structure was inconsistent at the instant of capture.

**Confirmed:** JIT info table photographed mid-fill. Settle check mitigates; expensive when hot. Disk store does not fix Class C.

**Falsified theories (do not revive without new measurement):**

- “Held critical sections” as the death — measured 0 came back held under CS tracking.
- “Mid-mutation alone suffices” — 109 mid-flight captures / 120 cycles produced 0 bad restores in that harness experiment; needs a second ingredient (e.g. missing writer thread).
- “ucrtbase is the game’s CRT” on Rabi-Ribi — observation (shared process heap) right; attribution wrong.

---

## 4. Top-down ownership map (Windows → this process → this engine)

```
 Windows kernel / executive
   │  threads, waits, APC/DPC completions, section objects, handles
   ▼
 ntdll / kernelbase / combase / win32u
   │  heaps (LFH, segments), TEB/PEB, loader, VEH, SRWLOCK, threadpool
   ▼
 Loaded modules
   │  Steam, overlay, XAudio2/dsound stacks, static CRT inside EXE, our proxy DLLs
   ▼
 Process heaps & mappings
   │  GetProcessHeap() shared by static UCRT + Windows + (today) game custom alloc
   │  MEM_MAPPED peers (Steam IPC, etc.) — often NOT in snapshot
   ▼
 Game logic + our d3d11/dxgi proxy + hooks
   │  rewound island: ~3–4 threads (game mains + xaudio2 when included)
   ▼
 Snapshot store (memory today / file proposed)
```

**Fighting you** means: an agent above or beside the rewound island that still has a live reference into the island, or that the island still believes is frozen when it is not.

---

## 5. Thread ownership — the island

### 5.1 Claim vs census

| Claim sometimes made | Measured / current practice |
|---|---|
| “We freeze the process” | Suspend is real, but **not all threads are rewound** |
| “System threads don’t matter” | Their TEBs held game heap blocks every session |
| Rewound set | **~3–4 threads** (main game threads + XAudio2 when added) |
| Left in present | ~dozen system/Steam/pool/etc. threads (order-of-magnitude from README) |

No LOCK lines from DLL interception tooling is **compatible** with this picture: many relevant locks are not IAT-visible CRITICAL_SECTIONs (SRWLOCK, Steam internals, audio). Absence of LOCK lines clears one story; it does not clear Class B peers.

### 5.2 Thread-set invariant

Save records N contexts; load may see M ≠ N (example: 55 → 52). Gone threads are Class C’s second ingredient: a structure whose writer will never finish. Export: `savestate_thread_set()` (built).

### 5.3 Ownership rule for threads

For each thread T:

1. If T’s stack/TEB/TLS are rewound ⇒ T is in the island.  
2. If T can run after resume with a present TEB ⇒ T is a **peer**.  
3. If a peer holds a pointer into rewound memory ⇒ Class B (veto that block, stop the peer, or rewind the peer).  
4. If the island holds a pointer into peer-only memory ⇒ Class A (retire, replay, or stop chasing).

Adding XAudio2’s thread to the rewind set is the same move as dsound cursor tracking: **pull the peer inside, or stop exchanging with it.**

---

## 6. Heap ownership — landlords on one arena

### 6.1 Rabi-Ribi fact (settled)

- EXE imports: `KERNEL32`, `USER32`, `GDI32`, `SHELL32`, `STEAM_API` only — **no CRT DLL**.
- Static UCRT in `.text`; heap handle is `GetProcessHeap()`.
- `ucrtbase.dll` in-process is **Windows’**, not the game’s.
- Log line “game runtime heap from ucrtbase” was a **misattribution** (fixed in PR scaffolding): right arena, wrong landlord story.

### 6.2 Closed Heap* surface (from `ghidra_surface.txt`)

**CRT set (must move together if private-heaped):**  
`_malloc` `0x36E6B2`, `_free` `0x369754`, `_realloc` `0x36E744`, `__calloc_impl` `0x37EBF7`, `_msize` `0x37805A`. **Five, not six.**
No `_expand`, no `_recalloc`. No `HeapCreate`/`HeapDestroy` — which is what settles that the static CRT takes `GetProcessHeap()`, independent of convention.

**Membership test:** reads `__acrt_heap` at `0x0118E588`. A function that merely *contains* a `HeapAlloc` call is not an allocator, and cannot be detoured as one — it has no pointer parameter to dispatch on. `FUN_00bdbe12` calls `GetProcessHeap` for itself and allocates, works and frees inside one call; it belongs below, not here.

**Co-tenants on the same process heap (not landlords, not allocators):**  
`FUN_00bdbe12`, `FUN_0087bc20`, `FUN_0087c070`, `FUN_008dfef0`. Each calls `GetProcessHeap()` for itself. `FUN_0087c070` takes no pointer argument at all — it reads a global mode flag, dispatches, and frees what it computed, which makes "custom free" the wrong name for it.

**Resolved, not settled-by-assertion.** An earlier revision held that a CRT-only private heap was incomplete isolation and that arming had to wait on proving the two sets never exchange blocks. That reasoning conflated co-tenancy with exchange — two allocators can share an arena forever without trading an object — and it missed that the risk is asymmetric. A co-tenant's block arriving at our `free` is already handled: the ownership check fails and it is forwarded to the original `free`. Only *our* block reaching a `HeapFree` we do not control could corrupt anything.

That direction is now impossible rather than unproven. All four co-tenants and the CRT's own `_free` reach `HeapFree` through **one import slot**; patching it puts an ownership check beneath every freer in the process, including unidentified ones. The census counts how many of our blocks arrive that way — zero over a long session *is* the proof that was being demanded, and non-zero is corruption caught rather than shipped. See `rabiribi_static_crt.md`.

**Relocatability is a separate constraint from verification.** A detour must displace bytes that contain no relative operand and end on an instruction boundary. `FUN_008dfef0` opens with `E8 3B 08 00 00`, a relative `CALL`; its first five bytes cannot be moved to a trampoline unchanged, and a 16-byte prologue comparison does not detect this. The five CRT functions each have a clean 7-byte prologue by that test.

### 6.3 Partition tactics the engine already uses

- Mark heaps ours / not-ours (`D3D9SW_REWIND_GAMEHEAP`, etc.).
- Block-level restore with system-reach veto (`blk_sys_mark`), including **direct-hit TEB veto** (not closure).
- `HeapValidate` oracles — pass even when later death occurs ⇒ bad *contents/peers*, not broken heap metadata (in those runs).

### 6.4 Ownership rule for heaps

You own a heap HANDLE iff **every** alloc/free/realloc/size entry point that produces or consumes its blocks is in your closed, redirected set — and no peer TEB/module root requires those blocks to stay present unless you veto them from restore.

Shared `GetProcessHeap()` without that closure ⇒ you own a **description**, not a landlordship.

---

## 7. Subsystem ownership (API hook ≠ ownership)

### 7.1 d3d11 / dxgi proxy (software path)

You load as `d3d11.dll` / `dxgi.dll`. In the software-raster path you **internalize** a large fraction of “graphics state.” Windows still owns real GPU/DXGI when forwarding, but many failure modes become “recreate / lose content” rather than “OS mixer thread holds my buffer.”

**Ownership character:** high internalization; residual holdouts are COM lifetimes, queries, composition, anything you left as true device objects.

### 7.2 DirectSound

Buffers look like game objects; the **device/mixer/notify graph** is a Windows peer. Cursor tracking + stop/seek is admitting shared ownership. Classic Class B if the mixer keeps running over rewound buffer identity.

### 7.3 XAudio2

More in-process than dsound (voices, callbacks), still a peer (engine thread, WASAPI endpoint). Including its thread in the rewind set is consistent with §5. Callbacks into voices whose owning heap was partitioned wrong ⇒ same class of death as dsound, different module names.

### 7.4 Steam / overlay

Imports `STEAM_API.dll`; overlay/client inject threads and often **MEM_MAPPED** IPC. Peers by default. Callbacks queued in Steam’s world and delivered onto game threads after restore are Class B delivery mechanisms.

### 7.5 ntdll / CRT / COM (cross-cutting)

| Peer mechanism | Class | Notes |
|---|---|---|
| Growable function tables | A | Fixed via record/replay |
| VEH list | A/B | Head vs nodes |
| SRWLOCK / wait-on-address | B | Hard to inventory |
| Threadpool (`Tp*`) | B | TEBs not rewound |
| COM stubs / GIT | A/B | Query retirement pattern |
| FLS/TLS destructors | B | Loader-owned |
| `atexit` / stdio / locale | A/B | Static CRT still has them in-image |
| Timer queues / multimedia timers | B | Present peer firing into past |
| Outstanding OVERLAPPED I/O | B | Kernel completes into rewound buffers |
| MEM_MAPPED skipped (~4.7 MB) | ? | Inventory required — unknown landlords |

### 7.6 LOCK lines

No LOCK lines from interception ⇒ no evidence of held CRITICAL_SECTION repair firing. **Not** evidence that locking peers are absent.

---

## 8. Store ownership — memory blob vs scratch-pad file

### 8.1 Problem

An in-process snapshot is another resident of the address space. It can appear in censuses, be reached from restored pointers, and compete for commit on the same heaps you are trying to reason about.

### 8.2 Scratch-pad file (proposed)

**Buys:** store is present-only by construction; durable hashable artifact; decouples slot size from game heap politics; better bisect (`cmp` two slots).

**Does not buy:** TEB/Steam/audio Class B; Class C mid-mutation; mapped IPC you still skip.

**Prefer:** unmapped `WriteFile`/`ReadFile` over `CreateFileMapping` until proven too slow — mapping reintroduces a MEM_MAPPED peer (ironically in the class you already under-capture).

**Hypothesis:** if deaths are peer-dominated (3–4 rewound threads), moving the blob to disk alone will not move the crash histogram much; if the blob self-interferes, it will.

### 8.3 Relation to `rewind_state_table.md`

Undo/keyframe designs multiply Class C sampling and thread-lifetime issues. Disk persistence is orthogonal and called out there as deferred. Ownership model here is prerequisite language for both.

---

## 9. Phase-by-phase: what we do vs what fights us

| Phase | We do | Fights us |
|---|---|---|
| **Attach / DllMain** | Proxy load; tiny init | Loader lock; must not heavy-hook |
| **First export** | Real init off loader lock | Steam/CRT may already have dirtied process heap |
| **Guard / hooks** | IAT / gated EXE patches; dsound track; CRT detours armed behind D3D9SW_GAMEHEAP=1, plus a HeapFree floor | Wrong target if you aim at ucrtbase's IAT on a static CRT; prologues must be relocatable, not merely verified |
| **Suspend** | Stop threads | Not all threads rewound; APC/completions; already-running peers |
| **Settle** | Wait writers out of hot modules | Cost vs coverage; Class C still possible |
| **Partition** | Ours/not-ours heaps; TEB direct veto; exclusions reachability | Dominates CPU; false attribution; contested inflation if veto too wide |
| **Capture** | Copy island + store | Mid-copy mutation observed even under suspend; MEM_MAPPED holes |
| **Resume after save** | Continue | Peers never stopped aging |
| **Restore writeback** | `0 WRONG` copy-complete | Class A/B still latent |
| **Fixups** | Audio seek, fntab replay, object retirement | Anything you did not inventory |
| **Invariants** | HeapValidate, thread-set, Mono oracles (harness) | Green oracles + later death ⇒ peer/content class |
| **Resume after load** | Island runs | **0 frames to death** cluster in the *harness* histogram. Rabi-Ribi is not clustered there: three sessions on 2026-09-12 died at 126, 0 and 498 frames, which is the shape of a latent corruption detonating on next access rather than an immediate wild pointer |

---

## 10. Settled claims (debate stoppers)

Use these as axioms unless new measurement overturns them:

1. **Copy-complete ≠ run-correct.** Byte-perfect writeback can precede instant death.  
2. **Inconsistency has three classes only: A, B, C.**  
3. **Rabi-Ribi statically links the CRT and shares `GetProcessHeap()` with Windows.** ucrtbase-in-process ≠ game CRT.  
4. **Co-tenancy on a heap is not exchange of blocks.** A private heap for the CRT set is sufficient once one ownership check sits under the shared `HeapFree` import slot; the earlier "incomplete isolation" claim assumed the answer to open question 2 below.  
5. **TEB closure veto over-excludes** (3317→7483 system-owned); direct-hit veto is the measured scale (~8).  
6. **Held-CRITICAL_SECTION theory was measured absent** in the harness configuration that tested it.  
7. **Deaths in the ss_ctx histogram were 0 frames after restore**, wild pointers, not “touched wrong side of cut” at the faulting address.  
8. **Exclusions/ownership analysis dominates save cost**, not memcpy.  
9. **Hook coverage ≠ ownership**; peers define ownership.  
10. **Scratch-pad file fixes store ownership, not peer ownership.**

---

## 11. Open claims (allowed arguments)

These are live; settle with measurement, not rhetoric:

1. Does TEB direct-hit veto move Rabi-Ribi / harness death rate?  
2. Do the co-tenants and the CRT exchange blocks? Now answered continuously by the `gameheap` census rather than by a separate tag experiment.  
3. What are the ~4.7 MB MEM_MAPPED sections by name/path?  
4. Exact rewind-thread roster vs full thread census (start address + module) on a real title-screen→play session.  
5. XAudio2: is rewind-thread enough, or are endpoint/callbacks still Class B?  
6. Scratch-pad: latency vs self-interference win on real slots.  
7. Whether any remaining death is Class C needing a narrower settle signal.

---

## 12. Decision procedure (when two explanations compete)

1. **Name the class (A/B/C)** or admit it is store/self. If you cannot, you do not have an explanation yet.  
2. **Name the peer** (thread, module, heap, mapped section, COM apartment).  
3. **Propose one of:** rewind peer, stop peer, veto block, retire object, replay registration, close allocator set, move store out of address space.  
4. **Predict a measurable delta** (system-owned count, contested, TEB veto M, death rate, mapped inventory).  
5. **Run the smallest experiment that can falsify it.**  
6. If the check has never failed, **force it to fail once** before trusting silence (`restore_invariants.md` rule).

---

## 13. Glossary

| Term | Meaning |
|---|---|
| **Island** | The rewound thread + memory set |
| **Peer** | Anything that can run or retain references outside the island |
| **Landlord** | Who allocates/frees a block’s heap |
| **Veto** | Mark block not restored though it sits on a “game” heap |
| **Copy-complete** | Writeback matched capture |
| **Run-correct** | Island executes without peer-induced inconsistency |
| **Store** | Snapshot cargo (memory or file) |
| **Holdout** | Peer or mapping that silently participates in ownership |

---

## 14. Bottom line

Windows is not “against” savestate. **Windows is full of peers.** This engine works when it tells the truth about the island: which threads, which heaps, which APIs, which mappings, which store.

Rabi-Ribi’s static CRT did not create a new physics; it removed a comforting lie about ucrtbase and showed a second landlord (`FUN_0087*`) on the same arena. Few rewound threads and no LOCK lines say the same thing from another angle: **the island is small; the shore is crowded.**

Future work — private heaps, scratch-pad files, rewind tables, more audio threads — should be argued only in this vocabulary: enlarge the island, shrink exchange with the shore, or move the diary off the beach.

---

*Document assembled 2026-09-12 for the rabiribi-savestate ownership debate. Prefer amending this file with new measurements over forking parallel “theories.”*
