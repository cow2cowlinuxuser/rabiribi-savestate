# Crash histogram — `ss_ctx.exe 30 4 mono`, 100 serial runs

Measurement only. Dataset is `chunk_01.txt` … `chunk_05.txt` produced by this batch. `d3d9_sw_savestate_ss_ctx.prebatch.txt` and every other host log were not parsed.

Per-fault rows: `crash_faults.csv`. Parser: `build_histogram.ps1`. Run log: `run_results.csv`.

## Batch summary

| | count | % of 100 runs |
|---|---:|---:|
| Runs attempted | 100 | 100% |
| Clean (stdout printed `done:`) | 50 | 50% |
| Died (no `done:`, not timed out) | 50 | 50% |
| Hung (150 s timeout, then killed) | 0 | 0% |
| VEH `fault:` records parsed | 49 | — |
| Unique runs that produced a VEH fault | 47 | — |
| Died with **no** VEH fault (silent) | 3 | 3% |
| Truncated-PC records set aside | 1 | — |
| Interleaved multi-fault sessions | 2 faults (runs 60, 81) | — |

**Exit codes are not usable.** `Start-Process` reported `ExitCode=0` for every run, including deaths that logged `C0000005` and never printed `done:`. Classification above uses stdout (`done:` vs not) and the hang timeout, not `ExitCode`. Raw values remain in `run_results.csv`.

**Silent deaths (runs 28, 50, 53).** Restore completed, heap check passed, then the process vanished with no `fault:` and no `exit:`. That is the fail-fast signature (`int 0x29` / `__fastfail`): the vectored handler never runs. Sites unknown. Loads completed before disappearance: 18, 11, 18.

`exit: ExitProcess called from ucrtbase.dll` appears on the 50 **clean** runs (normal CRT teardown). It is not a crash.

### Chunk accounting

Every chunk has 20 sessions = 20 runs. Log cap is 8 MB; largest chunk is 1.16 MB. No rotation-loss.

| chunk | runs | sessions | faults | bytes |
|---|---:|---:|---:|---:|
| chunk_01.txt | 1–20 | 20 | 10 | 1,050,982 |
| chunk_02.txt | 21–40 | 20 | 10 | 1,125,748 |
| chunk_03.txt | 41–60 | 20 | 9 | 1,100,684 |
| chunk_04.txt | 61–80 | 20 | 8 | 1,155,276 |
| chunk_05.txt | 81–100 | 20 | 12 | 1,075,758 |
| **total** | **1–100** | **100** | **49** | **5,508,448** |

100 sessions ↔ 100 runs. No missing or extra session.

---

## 1. Exception code

All 49 VEH faults are `C0000005` (access violation).

| code | count | % of 49 |
|---|---:|---:|
| C0000005 | 49 | 100% |
| C0000409 fail-fast | 0 logged | — |
| C0000374 heap corruption | 0 | 0% |
| C000000D / C0000094 / other | 0 | 0% |

The 3 silent deaths are the only candidates for fail-fast. That is an inference from absence of a VEH record, not a decoded subcode.

---

## 2. Faulting site (`module+offset`)

Primary histogram. ASLR: sites are `module+offset` only. One truncated-PC record (`unknown+0` at PC `0`) is included here for the module name, and excluded from offset/shape analysis below.

| site | count | % | symbol (cdb `ln` / `uf`) |
|---|---:|---:|---|
| ntdll.dll+C559 | 5 | 10.2% | `RtlpEnterCriticalSectionContended+0xd9` — `monitorx rax, rcx, rdx` |
| unknown+0 | 4 | 8.2% | not a module; RIP was unmapped (see shape) |
| ucrtbase.dll+3CCCB | 4 | 8.2% | `strchr+0x3b` — `pcmpeqb xmm1, xmmword ptr [rax]` |
| ucrtbase.dll+12E7A | 4 | 8.2% | `parse_integer<…>+0x1a` — `mov r14, qword ptr [rdx]` |
| ucrtbase.dll+1E558 | 3 | 6.1% | sprintf `output_processor::process+0x428` — `jmp rcx` |
| ucrtbase.dll+1E1A4 | 3 | 6.1% | same `process+0x74` — `movsx r8, byte ptr [r9]` |
| ucrtbase.dll+19CD8 | 3 | 6.1% | `output_processor::state_case_type+0x4c8` |
| ntdll.dll+C55C | 2 | 4.1% | `RtlpEnterCriticalSectionContended+0xdc` — `mov eax, dword ptr [r9]` |
| ucrtbase.dll+451E7 | 2 | 4.1% | `type_case_integer<10>+0x1f7` |
| ucrtbase.dll+19A76 | 2 | 4.1% | `state_case_type+0x266` |
| ucrtbase.dll+1E17B | 2 | 4.1% | `process+0x4b` — `inc dword ptr [rdi+468h]` |
| *(16 sites seen once)* | 16 | 32.7% | see one-offs |

By module:

| module | count | % |
|---|---:|---:|
| ucrtbase.dll | 33 | 67.3% |
| ntdll.dll | 9 | 18.4% |
| unknown | 4 | 8.2% |
| ss_ctx.exe | 1 | 2.0% |
| mono-2.0-bdwgc.dll | 1 | 2.0% |
| KERNEL32.DLL | 1 | 2.0% |

Microsoft symbols for ntdll / ucrtbase / KERNEL32 resolved from the symbol server. `mono-2.0-bdwgc.dll` is delay-loaded from the game tree; it was not mapped at the cdb initial breakpoint and did **not** resolve. Local PDB for `ss_ctx.exe` resolved.

---

## 3. Access kind

| kind | count | % | who-class |
|---|---:|---:|---|
| reading | 34 | 69.4% | 34 unmapped |
| writing | 11 | 22.4% | 7 unmapped, 4 into `ucrtbase.dll` (mapped image) |
| executing | 4 | 8.2% | 4 unmapped (`unknown+0`) |

No log line said `no-access` / guard. Unmapped vs “other” is the split the logger actually emits.

The 4 writes into `ucrtbase.dll` are a mapped, **held-in-the-present** system image (not rewound). They are the only faulting addresses that land in a real mapping.

---

## 4. Frames since the last restore

**49 / 49 = 0.** Every VEH fault says `0 frame(s) since the last restore`. Not “usually 0”. Always 0 in this batch.

Deaths are on the first thing the resumed thread executes after `load:`, not after a long run of post-restore work.

---

## 5. Real unwind depth and caller chains

`stack-scan GUESS` lines were ignored. Chains are `frame N` / `RtlVirtualUnwind` only.

| unwind depth | count |
|---:|---:|
| 0 (empty; interleaved, run 60) | 1 |
| 1 (`NOT_EXECUTABLE` — walk stopped) | 4 |
| 3 | 1 |
| 4 | 7 |
| 6 | 13 |
| 7 | 16 |
| 8 | 6 |
| 9 | 1 |

Recurring **full** chains (3+ identical sequences):

| count | chain |
|---:|---|
| 4 | `NOT_EXECUTABLE` |
| 4 | `ntdll.dll+C559 > ntdll.dll+D8E2 > mono-2.0-bdwgc.dll+70D07 > mono-2.0-bdwgc.dll+70CAB > ss_ctx.exe+2FFA > KERNEL32.DLL+2E8D7 > ntdll.dll+8C3FC` |
| 4 | `ucrtbase.dll+3CCCB > ss_ctx.exe+3063 > KERNEL32.DLL+2E8D7 > ntdll.dll+8C3FC` |
| 3 | `ucrtbase.dll+19CD8 > ucrtbase.dll+1E541 > ucrtbase.dll+18588 > ss_ctx.exe+124A2 > ss_ctx.exe+3238 > KERNEL32.DLL+2E8D7 > ntdll.dll+8C3FC` |
| 3 | `ucrtbase.dll+1E1A4 > ucrtbase.dll+18588 > ss_ctx.exe+124A2 > ss_ctx.exe+3238 > KERNEL32.DLL+2E8D7 > ntdll.dll+8C3FC` |

Family counts (substring, not a distinct-chain histogram):

| family | faults | our frame | meaning |
|---|---:|---|---|
| through `ss_ctx.exe+124A2` | 27 | `ss_ctx!sprintf+0x52` | CRT sprintf from `jitter` (`inflight_set` / `%d` into a tiny buffer) |
| through `ss_ctx.exe+2FFA` | 7 | `ss_ctx!jitter+0x1da` | `mono.get_method(...)` → ntdll critical section |
| through `ss_ctx.exe+3063` | 5 | `ss_ctx!jitter+0x243` | `class_is_generic` → `strchr` on `mono.class_get_name` |
| `KERNEL32.DLL+2E8D7` | most chains | `BaseThreadInitThunk+0x17` | thread entry |
| `ntdll.dll+8C3FC` | most chains | `RtlUserThreadStart+0x2c` | thread entry |

---

## 6. Address shape of the faulting address

Denominator: 48 live records (1 truncated-PC set aside: run 90, executing `0000000000000000`).

| shape | count | % of 48 | notes |
|---|---:|---:|---|
| null or small offset (&lt; 0x10000) | 28 | 58.3% | 21 were exactly `0`; also `+0x10`(2), `+0x18`(2), `+1`, `+0x30`, `+0xD8` |
| all-ones (`FFFFFFFFFFFFFFFF`) | 10 | 20.8% | log `SHAPE: high half FFFFFFFF, low half ALL ONES` |
| module-like | 4 | 8.3% | the 4 writes into `ucrtbase.dll` |
| half-pointer, low 32 = all ones | 3 | 6.3% | all three `00007FF7FFFFFFFF`; executing |
| other | 3 | 6.3% | not classified further |
| half-pointer, low 32 = zero | 0 | 0% | |
| stack | 0 | 0% | |
| heap-like | 0 | 0% | |

**Half-pointer surviving-half check (the 3):** high half `00007FF7` matches the high half of **this process’s `ss_ctx.exe` base** (`00007FF704BA0000`) on every run in the batch. It does **not** match RSP’s high half. Log `PATTERN: faulting address IS rsp with low 32 cleared`: **0 occurrences**.

The 10 all-ones addresses also printed the logger’s `SHAPE: low half ALL ONES` line (13 SHAPE lines = 10 all-ones + 3 half-ptr).

---

## 7. Boundary dimension

This is the question the batch was for. Short answer: **the faulting address does not cluster on the rewind/present cut.** The bad pointer is almost always unmapped garbage (NULL / all-ones / a half-pointer). What *does* recur is which *registers* still point at rewound memory when that garbage is used.

### 7a. The faulting address itself

| | count |
|---|---:|
| Faulting address annotated with a named heap | **0 / 49** |
| Faulting address `which we rewind` | **0** |
| Faulting address `which we leave in the present` | **0** |
| Faulting address `restored from the save` | **0** (unmapped targets are not in the save map) |
| Unmapped | 45 |
| Mapped image (`ucrtbase.dll`, held in the present) | 4 |

There is **no** clustering of the dereference target on one side of the boundary. The instruction is not dying because it touched a rewound page or a held page. It is dying because the address is not a page at all, except for those 4 stores into the CRT image.

### 7b. Registers reported at the fault

`ss_where_reg` only prints a GPR if it is ≥ 0x10000 and `VirtualQuery` says not `MEM_FREE`. So this is “registers that still look like pointers”, not all 16 GPRs.

Totals below are **46 live, non-interleaved faults** (2 interleaved sessions excluded because concurrent `ss_raw` lines can attach a register to the wrong fault; 1 truncated-PC fault also excluded). Inclusive-of-interleaved counts are a few units higher and do not change the shape.

| register annotation | count | % of 254 reported |
|---|---:|---:|
| side: `restored from the save` | 197 | 77.6% |
| side: `held in the present` | 55 | 21.7% |
| side: `in neither the save nor the held set` | 2 | 0.8% |
| in a heap we rewind (always “game's runtime”) | 29 | 11.4% |
| in a heap we leave in the present | **0** | **0%** |
| no heap annotation (stack or image) | 225 | 88.6% |

Grep of the chunks: **zero** register lines in fault blocks say `which we leave in the present`. Every heap-annotated GPR is the game runtime heap, `which we rewind`, `restored from the save`.

That 197/55 split is **not** evidence of a boundary bug by itself. Stacks are rewound, so RSP/RBP/R8-as-stack are almost always `restored from the save`. ntdll/ucrtbase images are left in the present, so a code address in RCX/RSI is `held in the present`. Both are what the partition table already advertised.

### 7c. Pairings that actually repeat

**Pairing A — ntdll critical section, 7 faults (`C559`×5 + `C55C`×2).**  
Chain is `RtlEnterCriticalSection` → `RtlpEnterCriticalSectionContended`, called from `mono+70D07` from `ss_ctx!jitter+0x1da` (`mono.get_method`). Fault is `monitorx` on RAX=0 / `mov eax, [r9]` with R9=0. Several GPRs are 0, so `THE FAULTING ADDRESS IS IN rax;rcx;rdx;…` is a logging artefact (every zero register matches a NULL target), not seven simultaneous carriers.  
On these faults **R14** (when printed) points at the **rewound game runtime heap**, restored from the save, and the first word of that object is `FFFFFFFFFFFFFFFF` (seen on the ntdll-family blocks in every chunk). The lock pointer is NULL; the heap object next to it looks like all-ones. That pairing is real and repeats. It is *not* “a held region indexing a rewound struct” — the CS pointer is simply 0.

**Pairing B — execute at `00007FF7FFFFFFFF`, 3 faults (runs 25, 36, 88).**  
RIP is a half-pointer: high half matches `ss_ctx.exe`, low half all ones. Access kind `executing`. Unwind stops (`NOT_EXECUTABLE`). Thread last parked in `KERNELBASE.dll`. **R12** points at the rewound game runtime heap, restored from the save. RSP is a restored stack. This is the closest thing in the batch to “a 64-bit code pointer with one half current and one half sentinel,” and it happened **three times**, not once.

**Pairing C — sprintf internals, 27 faults through `ss_ctx!sprintf`.**  
`jitter` formats the inflight row with `sprintf`. After restore, CRT `output_processor` reads a NULL/all-ones format/output pointer, or `jmp rcx` through a jump table with a garbage index. The `this` / format pointer lives on a **restored stack**; ucrtbase itself is held in the present. That is a restored-stack / present-image pairing, but the *value* being used is garbage, not a plausible present-side object.

**Pairing D — `strchr` on a Mono class name, 5 faults (`3CCCB`×4 + `3CCBE`×1).**  
`class_is_generic` does `strchr(mono.class_get_name(k), '`')`. `strchr` then `pcmpeqb` on `[rax]`. RAX is a bad string pointer. The caller is our worker; the name should have come from rewound Mono metadata.

### 7d. What is **not** here

- No faulting address sits in a named heap on either side of the cut.
- No register in a fault block points at a **present** heap (ntdll/msvcrt/unowned).
- No `PATTERN` of RSP with its low 32 bits cleared.
- No half-pointer whose surviving half matches RSP.
- The 4 writes into `ucrtbase.dll` are stores into a present image from a restored thread — interesting, **seen twice per site, four times as a class**. Not a 3+ site by itself except as that class.

**Plain statement:** this batch does **not** show deaths clustering on the rewind/present memory cut. It shows workers resuming 0 frames after restore and immediately using a NULL, all-ones, or half-pointer inside ntdll’s lock path, CRT sprintf/strchr, or (once) our own `jitter`. Heap-pointing registers that are still valid enough to print are always the rewound game heap, which is also where this workload allocates.

---

## 8. Cycle / timing

`loads_before_fault` = completed restores in that session (the `load:` line precedes the fault). 30 cycles were requested.

| restore index at death | faults |
|---:|---:|
| 1–10 | 15 |
| 11–20 | 21 |
| 21–29 | 13 |

Not concentrated in early cycles. A mild mid-batch bump, not a first-restore spike. 44 / 49 VEH faults happened before the stdout `25 cycles` line; 5 after. The 3 silent deaths were after 11, 18, and 18 restores — also not “cycle 1 only.”

Clean runs that finished all 30 still exist (50 of them). Death is not inevitable given enough cycles; it is a per-restore coin flip with roughly even odds in this configuration.

---

## 9. Thread identity

Park site = last `thread TID parked at … in PATH` before the fault (where the snapshot froze that thread).

| last park module | count | % |
|---|---:|---:|
| ucrtbase.dll | 33 | 67.3% |
| ntdll.dll | 9 | 18.4% |
| KERNELBASE.dll | 3 | 6.1% |
| ss_ctx.exe | 3 | 6.1% |
| KERNEL32.DLL | 1 | 2.0% |

| role heuristic | count |
|---|---:|
| restored-other (parked outside ss_ctx) | 46 |
| ss_ctx-parked (could be worker in `jitter` or main) | 3 |
| requester-likely (the unique ss_ctx-parked thread) | **0** |

The 3 `ss_ctx-parked` faults are: `ucrtbase+3CCBE` (strchr), `mono+3C460` (one-off), `ss_ctx+2F54` (`jitter` itself). All three chains end in `BaseThreadInitThunk`, i.e. a worker thread, not the save/load requester.

**Deaths in this batch are on worker threads**, frozen in CRT or ntdll (or, rarely, in `jitter`), not on the main thread that called `savestate_load`.

---

## Recurring signatures (seen 3+ times)

Kept separate from the one-offs on purpose.

1. **`ntdll!RtlpEnterCriticalSectionContended` on a NULL lock (7 faults: `+C559`×5, `+C55C`×2).** Worker in `mono.get_method` / `RtlEnterCriticalSection`. `monitorx` or `mov eax,[r9]` with a zero address. R14 often a rewound heap object whose first word is all-ones. Always 0 frames after restore.

2. **CRT sprintf from `jitter` (27 faults through `ss_ctx!sprintf+0x52`).** Top sites inside that family: `process+0x428` `jmp rcx` (3), `process+0x74` (3), `state_case_type+0x4c8` (3), `parse_integer+0x1a` (4). Bad format/output pointer (NULL or all-ones) on a restored stack.

3. **`ucrtbase!strchr+0x3b` from `class_is_generic` (4 at `+3CCCB`, plus 1 neighbour `+3CCBE`).** SIMD read from a bad name pointer returned by Mono.

4. **Execute at half-pointer `00007FF7FFFFFFFF` (3).** RIP = `ss_ctx` high half + all-ones low half. R12 in the rewound game heap. Parked in KERNELBASE. Unwind refuses.

## Seen twice (not promoted)

`ucrtbase+451E7`, `ucrtbase+19A76`, `ucrtbase+1E17B` (the last two are writes; `+1E17B` writes into the ucrtbase image). `ntdll+C55C` is listed under signature 1 rather than here.

## One-offs (seen once)

`ucrtbase.dll+ED171`, `+4505C`, `+3CCBE`, `+1E426`, `+19ECA`, `+19E9A`, `+EDC1F`, `+19A73`, `+19A68`, `+17066`, `ntdll.dll+AC77`, `+DF5C`, `mono-2.0-bdwgc.dll+3C460`, `KERNEL32.DLL+842F0` (`TlsGetValue+0x10`), `ss_ctx.exe+2F54`, and truncated `unknown+0` at PC 0.

### The one fault in our module — `ss_ctx.exe+2F54` (run 97, once)

cdb `uf` from the function start (`ss_ctx!jitter`), not from the offset:

```
mov     rax, qword ptr [g_held]
mov     ecx, dword ptr [rax+r14*4+0DCh]   ; g_held->seen[idx]
cmp     ecx, dword ptr [rax+0D8h]         ; FAULT — g_held->gen
```

Access: **reading `00000000000000D8`**. RAX was 0, so this is `[g_held->gen]` with a NULL `g_held` pointer in RAX. The Held object is excluded (held in the present); the **pointer** `g_held` lives in `ss_ctx.exe` `.data`, which is rewound. RCX at the fault held the PC itself (`side=neither`). R12 pointed at the rewound game heap. Frames since restore: 0. RIP was the compare, so the previous `mov ecx,[rax+…]` was not re-executed on this resume — either RAX was restored as 0 with RIP already on the `cmp`, or this is a one-off we must not turn into a story. **Seen once.**

---

## What this does NOT show

- Not the game. Four synthetic JIT workers against a real Mono, no Unity, no render thread.
- `D3D9SW_LOCKS=0` and `D3D9SW_VERIFY=0`. Lock repair off.
- 30 cycles, 4 threads, mono mode only. A different cadence is a different experiment (the harness comments already say a 20 ms gap vs none changed the failure rate).
- **Not a 33% death rate.** This batch was **50%**. Either the ~33% figure was a different build/config, or 100 runs is still a wide interval. Do not average them.
- Exit codes from this runner are fiction (all 0). Do not histogram them.
- 3/50 deaths have no site (fail-fast inference only).
- 1 truncated PC; 2 interleaved faults. Shape/chain on those are weaker.
- `stack-scan GUESS` frames were discarded; mixing them back in would recreate already-known false callers.
- Mono offsets `+70D07` / `+70CAB` / `+3C460` are **unsymbolised**.
- First words of heap objects were grepped from the log, not columns in `crash_faults.csv`.
- `ss_ctx.exe` did not rebase across these 100 runs, so `00007FF7` as a high half is this machine’s mapping, not a portable constant.
- Zero hangs. That may be this machine, this timeout, or this build. It is not a claim that hangs are gone.
- A negative boundary result is not a proof that the cut is healthy. It is a proof that **these 49 VEH deaths are not “touched the wrong side of a straddling struct.”** They are wild pointers used 0 frames after restore, mostly in CRT and ntdll, on worker threads.

---

## Files left behind

| file | what |
|---|---|
| `chunk_01.txt` … `chunk_05.txt` | the dataset |
| `chunk_accounting.csv` | runs per chunk |
| `run_results.csv` | per-run exit/hang/stdout_done (exit untrustworthy) |
| `runs/run_NNN.txt` | per-run stdout |
| `crash_faults.csv` | one row per VEH fault |
| `build_histogram.ps1` | parser |
| `run_crash_batch.ps1` | serial runner (used for this batch) |
| `symbolize_sites.cdb` / `symbolize_out.txt` | cdb `ln`/`uf` log |
| `d3d9_sw_savestate_ss_ctx.prebatch.txt` | earlier-build log, **not** in this dataset |
