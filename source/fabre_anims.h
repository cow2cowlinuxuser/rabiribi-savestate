/* Auto-generated FABRE walk subset - do not edit */
#ifndef FABRE_ANIMS_H
#define FABRE_ANIMS_H
#include <gctypes.h>

#define FABRE_SHEET_W 256
#define FABRE_SHEET_H 256
#define FABRE_SHEET_COUNT 1

typedef struct {
	s16 src_x, src_y, src_w, src_h;
	s16 dest_x, dest_y, dest_w, dest_h;
	s16 origin_x, origin_y;
	s8  layer;
	u8  flip_h;
	u8  sheet;
	f32 x_scale, y_scale;
	f32 rotation;
} FABREPart;

typedef struct {
	const char *name;
	u8 count;
	const FABREPart *parts;
} FABREPose;

static const FABREPart fabre_parts_BASE[] = {
	{ 116, 8, 12, 16, 7, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 116, 8, 12, 16, -7, -11, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 116, 8, 12, 16, -23, -11, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 24, 80, 20, 20, 24, -28, 20, 20, 10, 10, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 96, 60, -50, -54, 96, 60, 47, 53, 10, 0, 0, 1.0f, 1.0f, 0.0f },
};

static const FABREPart fabre_parts_PATTERN001[] = {
	{ 116, 8, 12, 16, 7, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 350.568f },
	{ 116, 8, 12, 16, -8, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 116, 8, 12, 16, -23, -11, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 8.82f },
	{ 24, 80, 20, 20, 24, -26, 20, 20, 10, 10, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 96, 60, -50, -54, 96, 60, 47, 53, 10, 0, 0, 1.0f, 0.9f, 0.0f },
};

static const FABREPart fabre_parts_PATTERN002[] = {
	{ 116, 8, 12, 16, 4, -9, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 313.092f },
	{ 116, 8, 12, 16, -10, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 324.756f },
	{ 116, 8, 12, 16, -26, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 332.82f },
	{ 116, 28, 28, 28, -10, -53, 28, 28, 14, 14, 0, 0, 0, 1.0f, 1.0f, 279.936f },
	{ 96, 80, 20, 20, 20, -28, 20, 20, 9, 9, 10, 0, 0, 1.0f, 1.0f, 258.408f },
	{ 8, 104, 96, 60, -53, -53, 96, 60, 48, 53, 10, 0, 0, 1.0f, 0.95f, 0.0f },
};

static const FABREPart fabre_parts_PATTERN003[] = {
	{ 116, 8, 12, 16, 5, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 329.94f },
	{ 116, 8, 12, 16, -6, -11, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 31.968f },
	{ 116, 8, 12, 16, -21, -11, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 2.196f },
	{ 24, 80, 20, 20, 23, -28, 20, 20, 10, 10, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 96, 60, -50, -54, 96, 60, 47, 53, 10, 0, 0, 0.95f, 1.0f, 0.0f },
};

static const FABREPart fabre_parts_PATTERN004[] = {
	{ 116, 8, 12, 16, 8, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 0.18f },
	{ 116, 8, 12, 16, -8, -9, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 345.168f },
	{ 116, 8, 12, 16, -23, -10, 12, 16, 5, 4, 0, 0, 0, 1.0f, 1.0f, 35.676f },
	{ 24, 80, 20, 20, 26, -28, 20, 20, 10, 10, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 96, 60, -50, -54, 96, 60, 47, 53, 10, 0, 0, 1.05f, 1.0f, 0.0f },
};

#define FABRE_POSE_COUNT 5
static const FABREPose fabre_poses[FABRE_POSE_COUNT] = {
	{ "BASE", 5, fabre_parts_BASE },
	{ "PATTERN001", 5, fabre_parts_PATTERN001 },
	{ "PATTERN002", 6, fabre_parts_PATTERN002 },
	{ "PATTERN003", 5, fabre_parts_PATTERN003 },
	{ "PATTERN004", 5, fabre_parts_PATTERN004 },
};

#endif
