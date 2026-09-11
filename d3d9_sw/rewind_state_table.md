# Rewind state table

A design for many cheap states instead of one expensive one, so rewind becomes a
step backwards rather than a reload.

Nothing here is built. Every number quoted as measured comes from
`d3d9_sw_savestate.txt` and `d3d11_sw.log` on 2026-08-30; every number quoted as
estimated is marked as such, and the estimates are the parts most likely to be
wrong.

## What we are copying from video, and what we are not

The idea came from H.264 frame types, and the analogy is load-bearing in one
direction and misleading in the other.

**Not applicable: interpolation.** A B-frame is reconstructed from a past and a
future reference because pixels can be averaged. Program state cannot. There is
no midpoint between two heaps, half a pointer is not a pointer, and a state we
never recorded cannot be synthesised from two that we did. Any design that
requires inventing an unrecorded state is dead on arrival.

**Applicable: reference frames, deltas, and replay order that differs from
record order.** That is the whole of the useful analogy, and it is enough.

## Three kinds of state

- **Keyframe (I).** A complete snapshot, standalone, no dependencies. What
  `do_save` produces today: 481 MB, ~750 regions, 55 threads, 45 contexts.
- **Forward delta (P).** The pages that changed since the previous state.
  Reaching a later state from an earlier one.
- **Reverse delta (U).** The *previous* contents of the pages that are about to
  change. Reaching an earlier state from a later one.

Reverse deltas are the important ones, and the reason is that they match the
operation we actually want. The present is always a complete state, held live in
the process itself. Stepping back one frame means applying one small undo record.
No reconstruction, no walking from a distant keyframe, and the cost is
proportional to what moved rather than to what exists.

This is what emulator rewind does, and what database undo logs do, for the same
reason.

## The state table

A ring buffer of entries, oldest evicted first:

```
keyframe  <- complete, taken every N frames
  U, U, U, U, ...   <- one reverse delta per frame after it
keyframe
  U, U, U, ...
```

Seeking to an arbitrary frame walks reverse deltas backwards from the present
while that is cheaper than restoring the nearest earlier keyframe and walking
forward deltas to the target. Keyframes exist only to bound how far backwards a
walk can get and to let old deltas be discarded.

Keyframes are also the only points at which the expensive per-save analysis has
to be redone - see the exclusion set below.

## The hard part: knowing which pages changed

Everything above is bookkeeping. This is the part that decides whether the design
is possible.

Windows will report dirty pages through `GetWriteWatch`, but only for regions
allocated with `MEM_WRITE_WATCH`. The game's heap was allocated by the game long
before we could ask for that, and there is no way to retrofit the flag onto
memory we do not own. So the choice is between two mechanisms we can implement
ourselves.

### Option 1: write barrier

Mark the pages read-only, catch the access violations in the vectored handler we
already have, record the old contents, restore write access.

Exact, and the cost is one exception per dirtied page per frame. 481 MB is about
123,000 pages; a frame that dirties even 2% of them is ~2,500 faults, and a
Windows VEH round trip is on the order of 5-20 us. **Estimated 12-50 ms per
frame**, which is unusable at 60 fps and unremarkable at 1 fps.

It also interferes. We would be changing the protection of memory the game owns,
underneath code that has its own exception handling and its own expectations
about what faults mean. Unity and Mono both use guard pages and both install
handlers. This is the riskier option by a wide margin.

### Option 2: parallel page hashing, against a shadow copy

Keep one shadow copy of the captured set. Each frame, hash every page and compare
against the previous frame's hash; pages whose hash changed are the dirty set.
Copy their old contents out of the shadow to form the reverse delta, then update
the shadow.

Reading 481 MB at a conservative 10 GB/s is ~48 ms single-threaded, and this is
embarrassingly parallel across the 16-thread raster pool we already own and
already stop during a save, so **estimated 3-6 ms per frame** for the hash pass.
Use a hardware CRC or similar; a 64-bit hash over a 4 KB page has a collision
probability small enough to ignore against every other failure mode in this
project.

Costs 481 MB of extra memory for the shadow, which the user has already said is
not a constraint.

**This is the recommended option.** It does not touch the game's memory
protections at all, its cost is predictable rather than dependent on the game's
access pattern, and it fails by being slow rather than by confusing somebody
else's exception handler.

## What the measured save cost says about all of this

The phase attribution is the most useful thing we have, and it points somewhere I
would not have guessed:

| phase | measured |
|---|---|
| suspend | ~30 ms |
| **exclusions** | **~340 ms** |
| release | 0 ms |
| walk | 15-25 ms |
| section + copy | 160-200 ms |

Copying is not the problem. The reachability audit inside the exclusions phase is
more than half the cost of a save - 1,477 root pointers, ~176,000 granules,
~524,000 interior edges, 266-297 ms of it. A delta scheme does not make that
cheaper, because it is not proportional to how much changed.

So the exclusion set must be computed **once per keyframe, not once per frame**,
and every reverse delta must reuse it. That is sound as long as the set only
describes what is ours versus what is theirs, which is what it does - but it is an
assumption that needs stating, because it silently becomes wrong if a heap is
created or a module is loaded mid-interval. Detecting that cheaply (heap count,
module count) and forcing a keyframe when it changes is the guard.

Per-frame budget at 1 fps, using measured numbers where they exist:

| step | cost |
|---|---|
| suspend 55 threads | ~30 ms measured |
| settle check | see below |
| hash pass, parallel | 3-6 ms estimated |
| copy dirty pages twice | proportional to churn |
| 45 thread contexts + TLS | small, part of the 30 ms today |

Comfortable against a 1,000 ms budget. Nowhere near a 16 ms one.

## Interaction with the hazards we already know about

This is the section that matters, because every one of these was found the
expensive way and a state table multiplies three of them.

**Mid-mutation capture, and why the settle check is a prerequisite.** On
2026-08-30 a crash was traced to Mono's JIT-info table being photographed during
a fill loop: a 380-entry chunk array, sane at indices 0-5, garbage at index 281,
which is in bounds. The table works for ordinary lookups and kills the process at
the next chunk split, which is the only operation that reads every entry. Nothing
was corrupt and no pointer crossed a boundary - both halves of the inconsistency
were on the same side, so no boundary could be moved to fix it.

Catching a thread mid-update is a **sampling problem**, and a state table samples
every frame instead of once a minute. This design therefore makes that class of
bug dramatically more likely, and the settle check is not an alternative to it but
a precondition for it.

There is an unresolved tension here. The deployed settle check waits 2 ms and
retries while any thread is anywhere inside Mono's module, up to 24 times. That is
free once a minute and unaffordable every frame. A per-frame scheme needs the
narrow version - the specific table-mutating functions rather than the whole
module - which means either symbol-derived offsets into a shipped binary or a
different signal entirely. **Open, and it gates per-frame operation.**

**Straddling pointers.** Known, and unchanged by this design: a pointer on the
rewound side naming memory on the held side. Confirmed instances so far are
Unity's `std::list<ID3D11Query *>` (fixed by object retirement), ntdll's
`RtlpDynamicFunctionTable` (fixed by recording and replaying the registrations),
and eight blocks held by system-thread TEBs (open, and about a kilobyte to fix).
Reverse deltas do not add to this class, because the boundary is in the same place
regardless of how the state is stored.

**Threads.** A thread that existed at keyframe time and has exited cannot be
resurrected; a restore has already been observed reporting 52 threads against 55
at save. Frequent states make this worse in one specific way: a long walk
backwards crosses more thread lifetimes than a single restore does. Unresolved,
and it may bound how far back a walk is allowed to go.

**Drift.** 22-70 MB per session of memory acquired after a state was taken,
currently left in place under policy 0. Per-frame states make drift per-frame,
which is probably an improvement - the interval is small enough that little
accumulates - but the interaction with keyframe eviction is unexamined.

## Staging

Each stage is useful alone and gated on the previous one being measured rather
than believed.

1. **Validate the settle check.** Does the JIT crash stop, and how many attempts
   does a save need? If attempts are routinely high, the whole design needs the
   narrow check before it can go per-frame. Nothing here is worth building until
   this is known.
2. **Shadow copy and hash pass, measured, doing nothing.** Allocate the shadow,
   hash in parallel on the raster pool, log dirty-page counts and pass duration
   per frame. No deltas, no rewind. This turns the two estimates in this document
   into numbers, and it is where the design is most likely to be found wrong.
3. **Reverse deltas, recorded and discarded.** Build the undo records and throw
   them away. Confirms the memory cost and the churn rate against real play.
4. **Single-step rewind.** Apply one undo record. The first point at which any of
   this is visible to a player, and the first point at which it can be wrong in an
   interesting way.
5. **The ring buffer and keyframe policy.** Interval, eviction, and the forced
   keyframe on heap or module count change.
6. **Backwards walks of more than one step**, which is where the thread-lifetime
   bound has to be faced.

## Deferred, with reasons

- **Attacking the 340 ms exclusions phase.** It dominates save cost, but
  per-keyframe reuse makes it a fixed cost per interval rather than a per-frame
  one, so it stops being on the critical path. Worth doing, not first.
- **Fixed-base arena for our own allocations.** Belongs to persistence across
  launches, not to rewind within a session. It would also make a wild pointer
  recognisable, which is a real if secondary benefit.
- **Persistence to disk.** Orthogonal. A state table is a prerequisite for it
  being cheap, not the other way round.
