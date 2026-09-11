#ifndef RBO_SCENE_H
#define RBO_SCENE_H

/*
 * Mode FSM drop-in for PC DAT_0256bd30 / FUN_00478390, aligned with
 * the live proto SCENE_* flow in main.c:
 *   CAUTION → LOGO → INTRO → TITLE → LOAD → LOBBY → STSEL → PLAY
 *
 * PC coarse modes (RE, approximate):
 *   0 title/boot-into-title, 1 opening/transition, 3 lobby/char/stage hub,
 *   4 result/ranking, 5 in-match (GameMain)
 *
 * main.c still owns the live switch; use these helpers when migrating.
 */

#include <gctypes.h>

/* Live proto scenes (keep numeric values stable with main.c). */
typedef enum {
	RBO_SCENE_CAUTION = 0,
	RBO_SCENE_LOGO,
	RBO_SCENE_INTRO,
	RBO_SCENE_TITLE,
	RBO_SCENE_LOAD,
	RBO_SCENE_LOBBY,
	RBO_SCENE_STSEL,
	RBO_SCENE_PLAY,
	RBO_SCENE_COUNT
} RboSceneId;

/* Coarse PC-style mode for future table-driven dispatch. */
typedef enum {
	RBO_MODE_TITLE = 0,
	RBO_MODE_OPENING = 1,
	RBO_MODE_UNUSED2 = 2,
	RBO_MODE_HUB = 3,     /* lobby / charsel / stagesel */
	RBO_MODE_RESULT = 4,
	RBO_MODE_BATTLE = 5
} RboModeId;

void rbo_scene_init(void);

RboSceneId rbo_scene_current(void);
void rbo_scene_set(RboSceneId id);

/* Map fine SCENE_* to coarse PC mode (best-effort). */
RboModeId rbo_scene_to_mode(RboSceneId id);

/*
 * Request transition; returns 1 if accepted.
 * Tracks identity only — callers still perform asset swaps.
 */
int rbo_scene_goto(RboSceneId id);

/* True if current fine scene belongs to PC hub mode (3). */
int rbo_scene_in_hub(void);

const char *rbo_scene_name(RboSceneId id);

#endif /* RBO_SCENE_H */
