#include "rbo_scene.h"

static RboSceneId s_cur;

void rbo_scene_init(void)
{
	s_cur = RBO_SCENE_CAUTION;
}

RboSceneId rbo_scene_current(void)
{
	return s_cur;
}

void rbo_scene_set(RboSceneId id)
{
	if ((int)id >= 0 && id < RBO_SCENE_COUNT)
		s_cur = id;
}

RboModeId rbo_scene_to_mode(RboSceneId id)
{
	switch (id) {
	case RBO_SCENE_CAUTION:
	case RBO_SCENE_LOGO:
	case RBO_SCENE_TITLE:
		return RBO_MODE_TITLE;
	case RBO_SCENE_INTRO:
		return RBO_MODE_OPENING;
	case RBO_SCENE_LOAD:
	case RBO_SCENE_LOBBY:
	case RBO_SCENE_STSEL:
		return RBO_MODE_HUB;
	case RBO_SCENE_PLAY:
		return RBO_MODE_BATTLE;
	default:
		return RBO_MODE_TITLE;
	}
}

int rbo_scene_goto(RboSceneId id)
{
	if ((int)id < 0 || id >= RBO_SCENE_COUNT)
		return 0;
	s_cur = id;
	return 1;
}

int rbo_scene_in_hub(void)
{
	return rbo_scene_to_mode(s_cur) == RBO_MODE_HUB;
}

const char *rbo_scene_name(RboSceneId id)
{
	static const char *names[] = {
		"CAUTION", "LOGO", "INTRO", "TITLE",
		"LOAD", "LOBBY", "STSEL", "PLAY"
	};

	if ((int)id < 0 || id >= RBO_SCENE_COUNT)
		return "?";
	return names[id];
}
