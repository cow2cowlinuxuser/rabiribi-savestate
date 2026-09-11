# Savestate engine audit

Read-only review of `savestate.c` and `ss_harness.c` against invariants A–G. Line numbers are from the tree as of this audit. I did not edit source, did not link, and did not run `ss_*.exe`. `zig cc -fsyntax-only -Wformat` could not be applied here (`FileNotFound` from the compiler on these paths); `ss_log` / `ss_raw` / `ss_fmt` also have no `__attribute__((format))`, so even a successful `-Wformat` pass would not have checked those call sites. Format/argument matching was done by hand.

**Headline.** Two things in this file can produce the signatures you are hunting. The first is a comment/code mismatch on thread stacks: the comment still describes the old “leave other threads’ stacks alone” policy, but the default is to rewind every game thread’s stack and `SetThreadContext` it, including threads parked in syscalls (`Sleep`, contended CS waits). `ctx_verify` cannot see the kernel reinstating its trap frame. That is invariant F, and it is the best match this audit found for both `RtlpEnterCriticalSectionContended` on a NULL lock and the half-pointer RIP family. The second is leftover CRT on harness workers (`strchr` in `class_is_generic`), which is exactly pairing D in the histogram. `ss_vfmt` itself is not the crash; it is mostly sound, with one latent width bug that no current call site hits.

---

## 1. Confirmed bugs

Ranked by how much they can explain a 50% death, 0 frames after restore, on workers.

### 1. Game-thread stacks are rewound under syscall trap frames (F) — severity: highest

The file’s own comment still describes a policy the code no longer implements:

```2850:2855:savestate.c
 * Stacks other than the requester's belong to threads suspended at arbitrary
 * instructions. SetThreadContext does not reliably take on a thread parked
 * inside a syscall - the kernel reinstates its own trap frame when the wait
 * completes - so a rewound stack under a live context is a wild return waiting
 * to happen. Those threads keep their own stacks and are left running; the
```

What the code actually does, default on:

```4874:4876:savestate.c
	n = GetEnvironmentVariableA("D3D9SW_REWIND_THREADS", v, sizeof(v));
	g_ctl->tmode = (n > 0 && n < sizeof(v) && (v[0] == 'o' || v[0] == 'O')) ? 0 : 1;
	return g_ctl->tmode;
```

Default `tmode` is 1 (rewind all). For a non-transient thread that then means:

```4488:4490:savestate.c
			if (g_ctl->ids[i] == g_ctl->req_tid || rewind_all_threads())
				continue;
			ss_exclude((void *)lo, (size_t)(hi - lo));
```

The `continue` skips excluding the committed stack. Contexts for those threads are saved and, on load, `SetThreadContext`’d. Transient classification is by **entry point** in an excluded module, not by current PC:

```4446:4448:savestate.c
			if (region_excluded((uintptr_t)g_ctl->starts[i], 1)) {
				g_ctl->transient[i] = 1;
				ss_exclude((void *)foot, (size_t)(hi - foot));
```

Harness workers start at `jitter` in `ss_ctx.exe`, which is rewound (`mine`), so they are **not** transient. Every iteration they park in a syscall:

```811:811:ss_harness.c
		Sleep(0);
```

`Sleep` is `NtDelayExecution` in KERNELBASE. Contended `RtlEnterCriticalSection` waits in ntdll (`NtWaitForAlertByThreadId` / similar). Histogram park sites for deaths: ucrtbase 67%, ntdll 18%, KERNELBASE 6%. Those are wait stubs, not `jitter`.

`ctx_verify` reads the context back **while the thread is still suspended**:

```5396:5399:savestate.c
 * Reads back with the same flags we set. A field that differs is not proof the
 * kernel refused it - a thread resumed between the set and the read would also
 * differ - but nothing is resumed until later in do_load, so within this window
 * a difference means the write did not land. */
```

That window cannot see the failure the comment at 2851–2853 names. GetThreadContext-while-suspended says the set took; ResumeThread lets the kernel put the trap frame back.

**What goes wrong.** Save-time stack bytes under a present-time (kernel) register file, or a mix: RIP in ntdll/KERNELBASE, RSP a restored stack whose frames belong to a different moment. Return from the wait uses a return address that is not the live call.

**Observed symptoms.**

- Pairing B (3 faults): parked in KERNELBASE, executing at `00007FF7FFFFFFFF` (ss_ctx high half + all-ones low), unwind `NOT_EXECUTABLE`. That is a 64-bit code pointer with one half a module and one half a sentinel, used as RIP after resume — the half-pointer family, loudly.
- Pairing A (7 faults): `RtlpEnterCriticalSectionContended` on a NULL lock, from `mono.get_method`, 0 frames after restore. A worker frozen in the contended wait is in a syscall. If the trap frame wins, RCX (the `CRITICAL_SECTION *`) is not the save-time argument; histogram has it NULL, with R14 in the rewound game heap whose first word is `FFFFFFFFFFFFFFFF`.

This is the only finding in the file that naturally produces **both** signatures.

### 2. `GetThreadContext` failure drops the context but not the stack (F) — severity: high

```5250:5251:savestate.c
		if (!GetThreadContext(g_ctl->handles[i], &t->ctx))
			continue;
```

No log. That thread is omitted from `s->threads`, so load never `SetThreadContext`s it. Its committed stack was still captured (same `rewind_all_threads` path as above) and will be written back. Stack rewound, registers left in the present: the exact inconsistency F forbids. Rarer than (1) on native x64 — GetThreadContext usually works even in a syscall — but silent when it happens.

### 3. CRT `strchr` still runs on snapshot-catchable workers (D) — severity: high for pairing D, not for the CS family

```607:609:ss_harness.c
		const char *n = mono.class_get_name(k);
		if (n && strchr(n, '`'))
			return 1;
```

`inflight_set` was converted off `sprintf`. This was not. It runs on every `jitter` iteration, so a snapshot catches it constantly. After restore it runs on a restored thread against a pointer from rewound Mono metadata.

Histogram pairing D: 5 faults at `ucrtbase!strchr` from `class_is_generic`, `pcmpeqb` on a bad `rax`. `strchr` itself is typically a lock-free SSE scan (so this is a weaker D than `sprintf` was), but it is still CRT on a restored worker, and it is where those five deaths landed. If `class_get_name` returned a good pointer into rewound metadata, `strchr` would likely survive; the crash is a **bad name pointer**. Treat this as the remaining CRT-on-worker site and as a victim of a wild pointer, not as the CS-NULL mechanism.

### 4. CRT `snprintf` inside the suspended window (A, D) — severity: high as invariant break, lower as the 50% death

```4502:4503:savestate.c
			off += snprintf(line + off, sizeof(line) - (size_t)off, "%s%s x%d",
					k ? ", " : "", onames[k], ocounts[k]);
```

This is in `build_exclusions`, which both `do_save` and `do_load` call after `suspend_all`. It is the old `ss_fmt` job, left behind when logging was moved to `ss_vfmt`. If `nown > 0` (any transient thread, and the harness has ntdll/thread-pool workers), it runs on every save and load with every other thread frozen.

A worker frozen inside `sprintf` / locale / stdio holds the CRT lock this call needs. That is the hang they already paid for with unwind-table `RtlFreeHeap`. The 100-run batch had **0 hangs**, so this is not the coin-flip death — but it is a real A+D break on the path they just spent a week making CRT-free, and it will hang the first time a worker is frozen in the matching lock.

`_strnicmp` in `module_excluded` is the same class, quieter, and runs for every module on every save/load while suspended:

```2876:2878:savestate.c
	if (wlen && _strnicmp(path, windir, wlen) == 0)
		return 1;
	if (_strnicmp(name, "steam", 5) == 0 || _strnicmp(name, "gameoverlay", 11) == 0)
```

### 5. `GetModuleFileNameA` while suspended (A) — severity: high as invariant break

The fault-handler comment already records that this API allocates:

```195:195:savestate.c
 * GetModuleFileNameA reaching RtlAllocateHeap from inside the report.
```

It is still called with the world frozen:

```5265:5266:savestate.c
				GetModuleFileNameA((HMODULE)mbi.AllocationBase, name,
						   sizeof(name));
```

That is **every game thread, every save**, after the copy, still before `resume_all`. Also `build_exclusions` at 4471 (new park sites) and `tally_owner` at 2973 (every transient). Same deadlock shape as (4). Same “0 hangs in the batch” caveat.

### 6. Uncached `GetEnvironmentVariableA` in the exclusions window (A) — severity: medium

They cached `verify_mode` because GEV writes a CRT scratch buffer that showed up as a changed region. These three still fire on **every** `build_exclusions`, uncached, while suspended, before the copy:

```3414:3414:savestate.c
	DWORD n = GetEnvironmentVariableA("D3D9SW_REWIND_GAMEHEAP", v, sizeof(v));
```

```3442:3442:savestate.c
	DWORD n = GetEnvironmentVariableA("D3D9SW_REWIND_SWHEAP", v, sizeof(v));
```

```3478:3478:savestate.c
	DWORD n = GetEnvironmentVariableA("D3D9SW_REWIND_ALLHEAPS", v, sizeof(v));
```

Also `drift_mode` (4536) and `reach_audit` / `reach_gran` during load/save exclusions. This is A (alloc/lock while frozen). It is not B: it happens before the copy, so the scratch write is photographed consistently. After restore that write is gone. Not the 50% death; still the exact API they already convicted.

`fntab_mode` / `settle_tries` use a `static int v = -1` latch and first-call GEV+`atoi` after suspend. They run **before** the copy on the first save of a session, so the initialized value is what gets snapshotted and later restores do not re-init. That is first-save A, not perpetual C. `verify_mode` is the one they primed at `ensure_helper` (6102) after learning C the hard way.

### 7. `ss_heap_of` still writes a static buffer during restore verify (B on the load path) — severity: low for current symptoms

Save-path verify deferred logging so `ss_heap_of` would not dirty the snapshot mid-compare. Restore-path verify did not:

```5754:5758:savestate.c
						ss_log("  VERIFY: region %d at %p did NOT come "
						       "back as saved - first difference at "
						       "+%llx, %llu word(s), heap %s\n",
						       i, base, fd, words,
						       ss_heap_of(s->regs[i].base));
```

```3628:3635:savestate.c
	static char buf[96];
	int i;

	if (!g_ctl)
		return NULL;
	for (i = 0; i < g_ctl->nheaps; i++)
		if (at >= g_ctl->heap_lo[i] && at < g_ctl->heap_hi[i]) {
			ss_fmt(buf, (int)sizeof(buf), ", in the %s heap at %p, which we %s",
```

This module’s `.data` is in the snapshot (exe is `mine`). The write is after `win_cmp` of the current region, so it does not false-fail that region. It can dirty already-restored `.data`. Scratch only; the dying batch had `D3D9SW_VERIFY=0`, so this path was off.

---

## 2. Invariant violations

Grouped by rule. Several items above are repeated here in one line so the map is complete.

### A — nothing in the suspended window may allocate, lock, or call the CRT

| Site | What |
|---|---|
| `savestate.c:4502` | `snprintf` (CRT format) in `build_exclusions` |
| `savestate.c:2876+` | `_strnicmp` (CRT locale) per module, same window |
| `savestate.c:5265`, `4471`, `2973` | `GetModuleFileNameA` (they already know it `RtlAllocateHeap`s) |
| `savestate.c:3414`, `3442`, `3478`, `4536`, `4123` | uncached `GetEnvironmentVariableA` |
| `savestate.c:4320` | `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` after suspend |
| `savestate.c:865` | `RtlDeleteGrowableFunctionTable` → `RtlFreeHeap` before restore (intentional vs double-free; still takes the heap lock) |
| `savestate.c:948` | `RtlAddGrowableFunctionTable` after restore, still suspended (they log “which allocates”; usually a no-op if Mono did not drop tables) |
| `savestate.c:5335` | `for_each_file` → `GetFinalPathNameByHandleA` before resume |
| `savestate.c:2805` | first `GetProcAddress("NtQueryInformationThread")` from `suspend_all` → `start_of` (loader lock) |

`ss_log` / `ss_raw` / `ss_vfmt` / `WriteFile` in this window are CRT-free by construction. `memcpy` / `memcmp` / `VirtualQuery` are fine. `heap_check` is correctly after `resume_all`.

The batch’s 0 hangs means a frozen thread rarely held the specific lock these APIs need. A is still broken.

### B — no writes between first region copy and last verify (save path)

Save-path verify defers `ss_log` / `ss_heap_of` until after the last `win_cmp`. `verify_mode` is primed at init so it does not write `.data` or a GEV scratch during that span. I did not find a remaining write on the **save** copy–verify span.

Load-path verify still calls `ss_heap_of` (static `.data` write) inside the copy loop — §1.7. Not the letter of B (B is the save path) but the same class of mistake.

### C — a latch inside the snapshot is not a latch

`ensure_helper` primes `verify_mode` and documents why (6097–6102). Function-local statics still in `.data` (exe is rewound):

| Latch | First use | After restore |
|---|---|---|
| `verify_mode` `cached` | primed at init | restored already-set; OK |
| `fntab_mode` `v` | `fntab_report`, before copy | snapshotted set; OK after first save |
| `settle_tries` `v` | start of `do_save`, before copy | same |
| `locks_mode` `v` | `cs_hook` at init | primed |
| `query_thread` `fn` | first `suspend_all` | snapshotted set; first save is A |
| `in_the_jit` `lo,hi` | settle loop, before copy | snapshotted; OK |
| `time_rewind` `enabled` | after restore in `do_load` | first load of a session re-inits (GEV while suspended, after restore) |
| `events` `enabled` | same | same |
| `ss_exclude` `said` | if list full | rewound, may log again |
| `fntab_keep_hooked` / `cs_keep_hooked` / `guard` `tick` | per frame | rate-limit resets; extra hooking, not a crash |

No latch I found re-initialises **during the save verify span** the way `verify_mode` used to. C is mostly paid down. The remaining first-use GEV/`GetProcAddress` costs are A, not perpetual C.

### D — no CRT formatting on a snapshot-catchable or restored thread

| Site | Status |
|---|---|
| `ss_log` / `ss_raw` / `ss_fmt` | routed through `ss_vfmt`; CRT-free |
| `savestate.c:4502` `snprintf` | still CRT, helper thread, **while others are frozen** |
| `ss_harness.c:608` `strchr` | still CRT, **on workers**, every compile |
| `ss_harness.c:287` `sprintf` in `inflight_open` | worker, but once at thread start, before the loop |
| `ss_harness.c:305–339` `inflight_set` | hand-rolled; this was the 27-fault `sprintf` family |

Harness `printf` on the main thread after restore is D in principle; deaths are on workers 0 frames after restore, before main prints.

### E — exclusion is whole region

`region_excluded` is overlap-based (2610–2621). `region_wanted` uses it on the full `VirtualQuery` region (2776). Callers that `ss_exclude` a stack tail `[foot, lo)` do **not** accidentally drop the committed stack: `lo < lo` fails the overlap test. Transient `[foot, hi)` correctly holds the whole stack. I did not find a site that assumes byte-granular exclusion against `region_wanted`.

`ss_exclude` of TEB `0x1000` holds the entire TEB mapping if it is one region — that is intended.

### F — thread context and stack must match

Broken by default for game threads in syscalls (§1.1) and by silent `GetThreadContext` failure (§1.2). Transient threads are consistent (stack held, registers untouched). Requester is consistent (stack captured, spinning in user mode, STC should take). Helper is not in the snapshot.

### G — a check that cannot fail is not a check

| Check | Blind to |
|---|---|
| `ctx_verify` | kernel trap-frame reinstatement at resume; only integer/control fields, not MXCSR/AVX; only while still suspended |
| save `win_cmp` | off when `D3D9SW_VERIFY=0` (the dying batch); unsuspended-thread re-enumeration only runs if `differ` is already true |
| `heap_check` | contents, not structure; TEB-side LFH caches; runs after resume |
| `cs_reconcile` | SRWLOCK / condvar; sections created before hooks (Mono `DllMain`); `LockCount == -1` as “free” on current ntdll (see Uncertain); **off** in the dying batch (`D3D9SW_LOCKS=0`) |
| `g_dc_*` decommit log | written in `.data` **before** the restore copy, then the copy wipes it (5688–5690 vs 5738). The fault-handler “decommitted drift” verdict cannot fire after a restore. Drift default is 0, so the path is off |
| CS “0 came back held” | if the held test is wrong, silence looks like a pass (already noted in `restore_invariants.md`) |

---

## 3. Uncertain

Each item says what would settle it. Not ranked as bugs.

### `CRITICAL_SECTION.LockCount == -1` as “unlocked”

```2036:2036:savestate.c
		if (!owner && cs->LockCount == -1)
```

Modern ntdll has used a waiter-bit encoding. If unlocked is no longer `-1`, every live section looks held, or (more likely given “0 held”) the test never sees a held section that is there. **Settle:** dump `LockCount` / `OwningThread` / `RecursionCount` for a section known to be held by a frozen worker, on this OS, without the restore. The dying batch had repair off anyway; this does not explain EnterCriticalSection(NULL).

### AVX / XSTATE not in `CONTEXT_FULL`

JIT workers use SIMD. `CONTEXT_FULL` is control+integer+segments+floating point (XMM), not YMM/ZMM. Unlikely to zero RCX or produce `00007FF7FFFFFFFF`. **Settle:** compare `GetThreadContext(CONTEXT_XSTATE)` before suspend and after resume on a worker that was in the JIT.

### TLS expansion slots

Only the first 64 TEB slots are copied (`TLS_MINIMUM_SLOTS`). Mono’s `thread_current` oracle passed in an earlier batch, so slot 0–63 are probably enough here. **Settle:** whether `mono_thread_current` after restore still matches if a worker has used expansion slots (it has not, in the measurements you already have).

### Collect-to-suspend gap / `SS_MAX_THREADS`

```4817:4818:savestate.c
			if (g_ctl->nids >= SS_MAX_THREADS)
				break;
```

Extra threads are never suspended and keep writing during the copy — torn 64-bit stores, the half-pointer shape `win_cmp`’s comment already names (4714–4717). Harness scale is far below 256. The gap between `collect_threads` and `suspend_all` is the same class for a thread created in that window. **Settle:** turn `D3D9SW_VERIFY=1` on; if copy-time diffs appear with “thread exists but was NEVER SUSPENDED”, this is in. The dying batch had verify off, so it could not have told you.

### `CreateToolhelp32Snapshot` / `VirtualAlloc` in exclusions

Snapshot and (if `D3D9SW_REACH_AUDIT=1`) two large `VirtualAlloc`s run while frozen. Default reach audit is off. **Settle:** whether a hang log stops in `build_exclusions` after `exclude:` vs before.

### `savestate_guard` vs a concurrent save

Guard bails if `g_ctl->busy` is set, then patches IATs. Harness calls guard only on main, which then spins inside `request()`. No race on the harness. A game Present thread vs a helper already in `do_save` is only a race if someone calls `savestate_guard` from a second thread after `busy` is clear and before the helper sets it — `request()` sets `busy` before the helper runs. Probably fine. **Settle:** a second thread calling `savestate_guard` during `do_save`.

### `g_held` NULL one-off (`ss_ctx.exe+2F54`)

Already in the histogram: RIP on the `cmp` against `g_held->gen`, RAX=0. `g_held` lives in rewound `.data`; the `Held` object is excluded. After restore the pointer should be the save-time (valid) value. RIP-on-cmp with RAX=0 is register/RIP desync (F), or a one-off. Seen once. Do not build a theory on it.

### `ss_vfmt` `%` at end of format / `LLONG_MIN` negation

`"foo%"` walks off the format string after `switch (*fmt++)` on NUL. No call site ends with `%`. `uv = (unsigned long long)(-v)` is UB for `LLONG_MIN`. No call site passes that. Not worth a bug row until a site exists.

---

## 4. Verified sound

### `ss_vfmt` / `ss_fmt` / `ss_digits` / `ss_frac`

Line-by-line, this is a clean formatter for the call sites it has. That is a useful result given it is new and on the fault path.

- **Buffer / `end`.** `p < end` before every store. `p` may equal `end`; returned length is `<= cap`. `ss_log`/`ss_raw` pass `sizeof buf` and `WriteFile` the count; no NUL required. `ss_fmt` passes `cap - 1` then writes `buf[n] = 0` with `n <= cap-1`. `cap <= 0` returns 0 without writing.
- **Padding.** `pad = width > total ? width - total : 0` cannot go negative. Loops are `pad-- > 0 && p < end`.
- **`%p`.** `va_arg(void *)` via `uintptr_t`; width `sizeof(void*)*2` (8 on x86, 16 on x64); zero-fill, no `0x`. Correct for both Windows ABIs.
- **Length modifiers.** `long` is 32-bit on both Windows targets; a single `l` correctly reads `int` / `unsigned int`. `%llu`/`%lld`/`%llx` increment `lmod` twice and read 64-bit. Call sites that pass `unsigned long long` use `%llu`/`%llx`; `(long)` uses `%ld`; `GetLastError()`-style values are 32-bit `unsigned long` with `%lu`/`%lx`. **No call site uses `%zu` or `%ju`.**
- **Latent width bug (not hit):** comment at 2407–2409 claims `%zu`/`%ju` widen. Code does `lmod++` once for `z`/`j`, then widens only if `lmod >= 2`. `%zu` would read `unsigned int` on x64. Do not add a `%zu` without fixing this (increment `lmod` by 2 for `z`/`j`, or special-case them).
- **Flags/width/precision.** `-` `+` `0` work. Space is consumed and ignored (no call site uses `% d`). `#` unsupported (none used). Integer precision is ignored (`%08X` uses width+zero, which is what the call sites want). String precision `%.80s` / `%.120s` works. `*` width is unsupported (none used). Width overflow to negative yields `pad = 0`, no overrun.
- **Float.** `prec` clamped 0–9. NaN/inf: `!(d < 1e15)` prints `"?"`. Negative inf prints `"-?"`. Rounding `+ 0.5` then split; `0.999` at prec 0 becomes `1`. Negative zero prints unsigned `0`. Scale overflow is the `1e15` guard. Fine for MB/ms diagnostics.
- **`ss_digits` / `ss_frac`.** `t[24]` holds uint64 decimal (20) and hex (16). `ss_frac` emits exactly `digits` characters, prec ≤ 9.
- **Default unknown specifier** emits `%` and does not consume a `va_arg` (would desync). All 105-odd sites use the supported set.
- **`%08X` / `%u` / `%d` with `unsigned long` / `long`:** Windows 32-bit `long`, same as `int` in `va_arg`. OK.

I would not hunt the 50% death in `ss_vfmt`.

### Other things that are doing what they claim

- **`ss_log` / `ss_raw`:** stack buffer, `ss_vfmt`, `WriteFile`. No CRT, no static format scratch. Safe in the window and on a restored thread.
- **`heap_check`:** after `resume_all` on both save and load. Will not deadlock on a frozen heap lock.
- **`fntab_reconcile_down` before any restore copy, `fntab_reconcile_up` after.** Ordering vs double-free is right. Unstick of the record lock before up is right. The remaining issue is A (heap lock), not order.
- **Transient vs game thread split, when the thread is not in a syscall:** entry in an excluded image → stack held, no context; otherwise (and `rewind_all`) stack captured, context saved. Internally consistent. The hole is the syscall case, not the classifier misfiring on `jitter`.
- **`region_wanted` / stack-tail exclude:** committed stack is a separate region from `[foot, lo)`; overlap math does not drop live frames.
- **`win_copy` / `win_cover`:** 64-bit section offsets split correctly into `MapViewOfFile` high/low DWORDs. `CreateFileMappingA` size split is correct.
- **TLS restore:** 64 slots at the documented TEB offsets (`0x1480` x64, `0xE10` x86), TEB itself excluded, slots written back after context set. Matches the “N with TLS” claim’s mechanism.
- **`rewind_all_threads` / `policy` / `reclaim_tier`:** latched in `g_ctl` (excluded), not in `.data`. Survive restore.
- **x86 stubs:** growable unwind tables, `patch_data_ptr`, `ss_where_reg` are 64-bit-only. x86 has no growable function tables; half-pointer family is x64. CS IAT hooks still run; CS data-pointer scan does not (weaker tracking, not a half-enabled crash).
- **`savestate_guard` vs helper on the harness:** main sets `busy` then spins; guard is not called from workers. `fntab_keep_hooked` / `cs_keep_hooked` do not run during the suspended window.
- **Format call sites vs `ss_vfmt` after the switch:** no `%zu`/`%ju`/`%n`/`%c`/`%e`. `%p` always a pointer. `%llu` always 64-bit. `%s` always `const char *`. Session timestamp `%u` with `WORD` promotes to `int`; values fit.

### Bounds when a limit is hit

| Limit | Behaviour |
|---|---|
| `SS_MAX_EXCL` | logs once, **drops** the range (held memory silently rewound) |
| `SS_MAX_THREADS` | extra threads never suspended (silent) |
| `SS_MAX_FNTAB` | extra tables unrecorded (silent); reconciliation can miss them |
| `SS_MAX_CS` | `overflow++`, logged at reconcile |
| `SS_MAX_REGIONS` | extra regions not captured (silent) |
| `SS_MAX_HEAPS` | `GetProcessHeaps` truncated to 64 |

None of these are near harness scale except possibly `SS_MAX_FNTAB` if Mono registered thousands of growable tables; the log line at save (`unwind tables: N registered`) would show it. Not silent in that sense.

---

## What this does not explain, on purpose

The 27-fault `sprintf` family from `inflight_set` is already fixed in the tree I read. This audit is of that tree. Pairing D (`strchr`) is not. Pairing A (CS NULL) and pairing B (half-pointer RIP / KERNELBASE) are the ones §1.1 is for.

I did not find a torn 32-bit store in `savestate.c` that would assemble `00007FF7FFFFFFFF`. The half-pointer shape on RIP after a KERNELBASE park is what you get when a restored stack and a kernel trap frame disagree, not from our pointer arithmetic truncating a 64-bit address to 32 bits. Region sizes logged as `%lx` / `(unsigned long)` truncate in the **log** only; the copy uses `uintptr_t`.
