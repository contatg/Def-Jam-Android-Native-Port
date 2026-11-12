/* Save states of a fight, for rollback netcode (docs/research/rollback-state.md,
 * docs/research/rollback-step-structure.md).
 *
 * The fight's state is the game's heap (one block in the 0x80000000 window) and the XBE's
 * .data/.bss, less what other threads own: the sound engine and its mixer (found at start-up),
 * the 60 Hz frame-record queue, and every page another thread writes while the game thread is
 * held for a moment at the match's first step. A save copies .data and the heap pages known to
 * change; which pages change is learned while the match runs, against a reference copy taken at
 * its start. Saves and loads happen on the game thread between two game updates.
 *
 * Windows only, like netplay. */
#ifndef DEFJAM_ROLLBACK_H
#define DEFJAM_ROLLBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROLLBACK_SLOTS 16

/* RECOMP_ROLLBACK=on: rollback is used (build 17 ships it off). */
int rollback_enabled(void);

/* The audio subsystem's start-up (sub_0003C6E0) begins / the next registry store ends it. */
void rollback_audio_init_begin(void);
void rollback_audio_init_end(void);
/* The sound engine's allocator (vtable 0x289480 +0 / +4), as recomp_lookup_manual hands it out. */
void rollback_sound_alloc(void);
void rollback_sound_free(void);
/* sub_00029500's first test: 1 when the block was taken from the arena (the audio code asked). */
int rollback_route_alloc(void);
/* A dynamic load (sub_0010AC20) begins; `caller` is its return address. */
void rollback_note_dynamic_load(uint32_t caller, uint32_t handle);
unsigned rollback_dynamic_loads(void);      /* how many so far (a clock for slow rollbacks) */
/* Tests (RECOMP_ROLLBACK_CHECK=1): the game allocator returned block for size bytes; says so when it
 * lies over the tables of the last loaded object, or those tables no longer hold. */
void rollback_check_alloc(uint32_t caller, uint32_t size, uint32_t block);
void rollback_check_tables(const char *where);
/* Called by the crash report (src/main.c) when set: netplay prints the rollback state. */
extern void (*rollback_crash_cb)(void);
const char *rollback_page_state(uint32_t addr);
/* The game heap's free (sub_0002A180), as recomp_lookup_manual hands it out. */
void rollback_heap_free(void);
/* The mixer's lock and unlock (0x2E61C0 / 0x2E61C4 point at one of two pairs). */
void rollback_mixer_lock_a(void);     /* sub_0011FCC0 */
void rollback_mixer_lock_b(void);     /* sub_00121C80 */
void rollback_mixer_unlock_a(void);   /* sub_0011FCD0 */
void rollback_mixer_unlock_b(void);   /* sub_00121C90 */

/* A match's first step, on the game thread: hold it for a moment and note every page the
 * other threads write meanwhile. Returns 0 when the save states are usable. */
int rollback_prepare(void);
/* At a clean point after rollback_prepare: the reference copy; pages known to change restart. */
int rollback_reference(void);
/* Compare 1/fraction of the pages not yet known to change with the reference. */
void rollback_discover(int fraction);
/* The calling thread enters (1) or leaves (0) the sound engine: its first writes meanwhile are
 * the engine's memory, which is not saved. */
void rollback_sound_context(int on);
int rollback_ready(void);
int rollback_save(int slot);
/* Private current-state checkpoint for a failed reexecution. */
int rollback_checkpoint131(void);
int rollback_restore_checkpoint131(void);
void rollback_drop_checkpoint131(void);
int rollback_capable131(void);
int rollback_load(int slot);
void rollback_reset(void);
/* For the log and tests: KB a save copies, pages known to change, pages left alone. */
void rollback_stats(unsigned *save_kb, unsigned *changing, unsigned *skipped);

#ifdef __cplusplus
}
#endif
#endif

