# One-way valves

The pass-1 restore philosophy, applied uniformly: **do not carry state across
the rewind — carry identity, then reconstruct or synthesize the rest.** Every
resource is a one-way valve. It either rebuilds or it doesn't come back, and
nothing the game holds is ever allowed to dangle.

## The invariant this is built to satisfy

> Every handle the game still holds after a restore must resolve to a
> structurally valid object of a compatible type. **Contents are free.**

Survival, not fidelity. A wrong picture or a beat of silence is acceptable; a
dangling pointer or an orphaned callback is not. Wrong content self-heals the
moment the game next renders or re-submits; a dangling handle crashes. So every
valve keeps *identity* present-tense (excluded from rewind, address stable) and
treats *contents* as disposable.

## The three columns

Each resource is sorted into exactly one treatment:

- **carry-as-identity** — the object's pointer/handle is present-tense
  (`savestate_exclude`d): it survives a restore unchanged so the game's held
  pointer stays valid. Its *state* is not rewound.
- **reconstruct-faithfully** — rebuilt exactly, from data the game already owns
  in its (rewound) memory. Zero fidelity loss, nothing extra carried.
- **synthesize-on-restore** — replaced by a structurally valid stand-in whose
  contents are arbitrary. Comes back as *something*; self-heals when the game
  next drives it.

## The valve table

| Resource | Treatment | Rule |
|---|---|---|
| **Audio engine (IXAudio2)** | carry-as-identity | Pointer is present-tense. Never recreated by the game, so it must survive. |
| **Audio voices** | carry-as-identity + flush | Pointers present-tense; on restore **flush to empty and stopped**. Not rewound. The game re-drives them. |
| **Queued PCM / sample counts / clock continuity** | does-not-come-back | Dropped. This deletes the rewindable-clock + arena-voice machinery in `xa2_sw.c` that existed only to make audio survive. |
| **Pending audio callbacks** | does-not-come-back | Dropped at the park (`cb_drop` already does this). This is the fix for the `DFD777C1` crash — the orphaned-callback path is gone by construction. |
| **Shaders (VS/PS)** | reconstruct-faithfully | Recompile from the game's DXBC bytecode (in rewound memory). Perfect, free, immutable. The archetype every other resource aspires to. |
| **Textures (display-only)** | synthesize-on-restore | Handle identity present-tense; contents = shader-filled stand-in of a compatible shape. Wrong picture, right size, no crash. |
| **Static vertex/index buffers** | synthesize-on-restore | Same: valid object, arbitrary contents, self-heals on next upload. |
| **Render targets / backbuffer** | synthesize-on-restore | Cleared/filled surface. The game re-renders these every frame anyway. |
| **Dynamic VBs / constants / render + sampler state** | does-not-come-back | Pure transient; the game re-sets all of it on frame 1 post-restore. |
| **The GPU device itself** | reconstruct (fresh) | New device on restore; never carried. All of the above are rebuilt/synthesized against it. |
| **Window / present surface** | carry-as-identity | Present-tense handle; framebuffer regenerated next frame. |
| **Pico worker threads (e.g. mixer)** | does-not-come-back | Ours; parked before copy, restarted after. Never rewound. |
| **Game threads with state** | reconstruct-faithfully | Context restored from the snapshot; `savestate_thread_set` tracks the save-vs-live set. |

## GPU: synthesize-any-handle, in detail

On restore, the GPU comes back through **one uniform loop, no special cases**:

1. Create a fresh real device (pico-owned).
2. Recompile every shader from the game's bytecode. (faithful)
3. For every other live handle, materialize a structurally valid stand-in:
   a shader fills it — solid colour, gradient, noise, a 1×1 default, anything.
4. Resume. Wrong frames self-heal as the game re-renders and re-uploads.

Two ways to know the stand-in's shape, pick one:

- **Descriptor-carried:** keep each resource's `w/h/fmt` header (the shadow
  already has it in `SwTexture`); drop only `pixels`. Stand-ins match dimensions.
- **Fault-in / lazy:** carry only identity; start every handle cold; the first
  bind or draw that touches a cold handle materializes a default (1×1 that
  inflates when a declared size appears). Smallest possible carry — "come back
  as something" taken literally.

Contents never cross the boundary either way, which is the point: it keeps
texture data off the rewind partition entirely, so it can never reintroduce the
cross-line-pointer questions the audio valve just eliminated.

## The one exception — readback into logic

Synthesizing content is safe **only for resources the game merely displays.** If
the game ever reads GPU content back into its own decisions —
`GetRenderTargetData` / staging-surface reads, occlusion queries, a compute
result it branches on — a stand-in feeds *wrong data into the simulation*, which
is a logic bug that does not self-heal.

Rule: **display-only → synthesize freely; ever-read-back → carry for real, or
prove the game never branches on it.** For a 2D DxLib sprite title this is
expected to be a non-issue (it renders and presents; it does not sample its own
output to decide what happens next), but it must be confirmed, not assumed —
grep the intercepted call log for any readback path before trusting the valve
blanket-wide.

## Gating test — the audio valve

Before trusting flush-on-restore, one behaviour decides it:

> **Does BGM resume after the audio engine is flushed on restore?**

DxLib streams music through `IXAudio2VoiceCallback` (`OnBufferEnd` →
`SubmitSourceBuffer`), so a voice left *started* with a live callback should get
its next pass-start request and refill within a frame or two. Confirm this. If a
track is instead submitted once-and-looped without a callback, that voice stays
silent until the next level load and needs a "re-arm the BGM voice" nudge on
restore. This is the only audio-valve risk; everything else about it is a
simplification.

## What this buys pass 1

- The `DFD777C1` crash class is gone by construction (callbacks dropped, not replayed).
- `xa2_sw.c` sheds its survival machinery (rewindable clock, arena voice state).
- GPU restore is one uniform loop; no static-vs-dynamic split, no data carried.
- Texture contents leave the rewind partition entirely.
- Worst case degrades to *tolerable leaks* (a stranded present-tense voice or
  resource, à la the VA-leak the ledger already tolerates) instead of crashes.

Continuity — real audio position, correct pixels on the first frame back — is a
later pass, added only where a test says the drop is visibly ugly.
