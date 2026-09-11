/* Auto-generated SPORE walk subset - do not edit */
#ifndef SPORE_ANIMS_H
#define SPORE_ANIMS_H
#include <gctypes.h>

#define SPORE_SHEET_W 256
#define SPORE_SHEET_H 256
#define SPORE_SHEET_COUNT 2

typedef struct {
	s16 src_x, src_y, src_w, src_h;
	s16 dest_x, dest_y, dest_w, dest_h;
	s16 origin_x, origin_y;
	s8  layer;
	u8  flip_h;
	u8  sheet;
	f32 x_scale, y_scale;
	f32 rotation;
} SPOREPart;

typedef struct {
	const char *name;
	u8 count;
	const SPOREPart *parts;
} SPOREPose;

static const SPOREPart spore_parts_BASE[] = {
	{ 124, 8, 124, 68, -60, -111, 124, 68, 21, 42, 0, 0, 0, 1.0f, 1.0f, 3.06f },
	{ 124, 88, 120, 60, -59, -94, 120, 60, 61, 32, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, -30, -37, 24, 36, 9, 8, 15, 1, 0, 1.0f, 1.0f, 71.244f },
	{ 8, 12, 56, 68, -28, -62, 56, 68, 29, 6, 108, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, 5, -38, 24, 36, 13, 10, 126, 0, 0, 1.0f, 1.0f, 284.832f },
};

static const SPOREPart spore_parts_PATTERN001[] = {
	{ 124, 8, 124, 68, -60, -104, 124, 68, 21, 42, 0, 0, 0, 1.0f, 1.0f, 3.06f },
	{ 124, 88, 120, 60, -59, -86, 120, 60, 61, 32, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, -31, -34, 24, 36, 9, 8, 15, 1, 0, 1.0f, 1.0f, 86.616f },
	{ 8, 12, 56, 68, -28, -56, 56, 68, 29, 6, 108, 0, 0, 1.05f, 0.9f, 0.0f },
	{ 80, 12, 24, 36, 5, -36, 24, 36, 13, 10, 126, 0, 0, 1.0f, 1.0f, 276.156f },
};

static const SPOREPart spore_parts_PATTERN002[] = {
	{ 124, 8, 124, 68, -60, -111, 124, 68, 21, 42, 0, 0, 0, 1.0f, 1.0f, 3.06f },
	{ 124, 88, 120, 60, -59, -94, 120, 60, 61, 32, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, -30, -37, 24, 36, 9, 8, 15, 1, 0, 1.0f, 1.0f, 123.12f },
	{ 8, 12, 56, 68, -28, -62, 56, 68, 29, 6, 108, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, 5, -38, 24, 36, 13, 10, 126, 0, 0, 1.0f, 1.0f, 240.516f },
};

static const SPOREPart spore_parts_PATTERN008[] = {
	{ 124, 8, 124, 68, -60, -115, 124, 68, 21, 42, 0, 0, 0, 1.0f, 1.0f, 3.06f },
	{ 124, 88, 120, 60, -59, -98, 120, 60, 61, 32, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, -30, -41, 24, 36, 9, 8, 15, 1, 0, 1.0f, 1.0f, 79.38f },
	{ 8, 12, 56, 68, -28, -66, 56, 68, 29, 6, 108, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, 5, -42, 24, 36, 13, 10, 126, 0, 0, 1.0f, 1.0f, 284.976f },
};

static const SPOREPart spore_parts_PATTERN009[] = {
	{ 124, 8, 124, 68, -60, -116, 124, 68, 21, 42, 0, 0, 0, 1.0f, 1.0f, 3.06f },
	{ 124, 88, 120, 60, -59, -98, 120, 60, 61, 32, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, -30, -38, 24, 36, 9, 8, 15, 1, 0, 1.0f, 1.0f, 121.788f },
	{ 8, 12, 56, 68, -28, -63, 56, 68, 29, 6, 108, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 12, 24, 36, 5, -39, 24, 36, 13, 10, 126, 0, 0, 1.0f, 1.0f, 244.008f },
};

#define SPORE_POSE_COUNT 5
static const SPOREPose spore_poses[SPORE_POSE_COUNT] = {
	{ "BASE", 5, spore_parts_BASE },
	{ "PATTERN001", 5, spore_parts_PATTERN001 },
	{ "PATTERN002", 5, spore_parts_PATTERN002 },
	{ "PATTERN008", 5, spore_parts_PATTERN008 },
	{ "PATTERN009", 5, spore_parts_PATTERN009 },
};

#endif
