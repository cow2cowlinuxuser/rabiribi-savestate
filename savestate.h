#ifndef D3D9_SW_SAVESTATE_H
#define D3D9_SW_SAVESTATE_H

#include <stddef.h> /* size_t, for savestate_exclude */

/* In-process rewind. Must be called from the thread that is inside Present,
 * at a point where the rasteriser has already flushed. Returns 1 on success.
 *
 * Scope is deliberately narrow: rewind within a single session, held in RAM,
 * never written to disk. That removes handle re-creation, which is the part
 * that makes general process checkpointing intractable on Windows. */
/* Redirects the game's clock and event imports. Idempotent, and called at the
 * first D3D entry point so events created during start-up are still seen. */
void savestate_hooks_install(void);

/* Records a redirection we have installed, so the inventory can report what this
 * wrapper is in a position to observe. Declared here because the hooks are
 * spread across several translation units - dsoundhook.c among them - and the
 * whole point of the record is that no single file knows all of them. */
void ss_hook_note(const char *kind, const char *what, void *at, int sites);

int savestate_save(int slot);
int savestate_load(int slot);
int savestate_slot_valid(int slot);
/* Cheap, call once per frame. Reclaims addresses the snapshot still needs
 * before another allocator can take them. */
void savestate_guard(void);
double savestate_last_ms(void);
double savestate_last_mb(void);
/* Nonzero if the operation that just completed was a restore, whichever of
 * savestate_save and savestate_load the caller invoked. A thread restored from a
 * snapshot resumes inside the save it was taking and returns through it, so a
 * successful restore arrives back in the caller's save branch. */
int savestate_last_was_restore(void);
/* Walks every heap's block headers and logs what is in them, comparing against
 * the session's first census. Observation only - it neither saves nor restores,
 * and that is what makes it able to take the heap's lock safely. Take one before
 * a scene transition and one after to see what the transition actually did. */
void savestate_census(void);
/* Milliseconds since the session's first restore; negative before there is one.
 * Never reset by later restores - it is a survival time, not a lap timer. */
double savestate_live_ms(void);

/* Holds a range in the present: never captured, never rewound.
 *
 * For state that has to survive a restore in order to observe one. A restore
 * rewinds every thread, including the one that asked for it, so anything a caller
 * keeps on its stack or in rewound memory travels back with the program - a
 * counter incremented across restores simply returns to its old value, and a loop
 * driven by one can never reach its second iteration. The test harness hit exactly
 * that and could not advance past cycle one.
 *
 * Register before the first save. Ranges added here are permanent and survive
 * every later rebuild of the exclusion list. */
void savestate_exclude(void *p, size_t bytes);

/* A setting's value, environment first and then d3d9_sw.cfg beside the log.
 * Returns the length written, 0 if unset.
 *
 * Exposed because the wrapper needs knobs the same way this engine does, and
 * for the same reason: a game launched through Steam does not inherit variables
 * typed into a shell, so plain getenv silently ignores whatever the user set.
 * Every knob that has to be settable in practice should come through here. */
unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

/* True when the main executable has this file name.
 *
 * Parts of this wrapper carry offsets and byte patterns that only mean anything
 * inside one game's binary. Nothing used to check which binary it was in, and
 * both places that do this were found reading and writing a stranger's image
 * within minutes of a test harness existing: the decoder patch turned a jne
 * into a jg after a nine-byte pattern matched by coincidence, and the dsound
 * cursor report dereferenced exe+0x50E18C in a 363 KB executable and faulted.
 * Call this before touching anything whose address came from a disassembly. */
int savestate_host_is(const char *exe_name);

/* Whether an address sits in a heap this engine takes back in time.
 *
 * 1 rewound, 0 left in the present, -1 not in any heap we know about. Writes
 * the heap's owner into name when there is one.
 *
 * For crash reports. Nearly every failure after a restore is a pointer whose
 * target sits on the other side of the rewind from the code holding it, and
 * which side is which decides the fix - so a fault handler that can name the
 * side turns a guess into a reading. Allocates nothing and takes no lock, so it
 * is safe from a handler that may be holding the heap lock already. */
int savestate_rewinds(const void *p, char *name, unsigned cap);

/* Thread-set mismatch after the most recent restore.
 *
 * The restore already compares the live thread set against the saved one and
 * logs the difference, but until now the counts lived only in the log. This
 * exports them cheaply so the harness can assert on them.
 *
 * Returns: count of threads present at save time that have exited.
 * Optionally fills *fresh (live now, not at save), *recycled (same entry point
 * under a new TID), *gone (same as the return value). */
int savestate_thread_set(int *fresh, int *recycled, int *gone);

/* One slot. Each costs a full copy of the game's committed memory, which for
 * this title is well over a gigabyte of physical RAM. */
#define SAVESTATE_SLOTS 1

#endif
