/* Auto-generated PORING walk subset - do not edit */
#ifndef PORING_ANIMS_H
#define PORING_ANIMS_H
#include <gctypes.h>

#define PORING_SHEET_W 512
#define PORING_SHEET_H 512
#define PORING_SHEET_COUNT 1

typedef struct {
	s16 src_x, src_y, src_w, src_h;
	s16 dest_x, dest_y, dest_w, dest_h;
	s16 origin_x, origin_y;
	s8  layer;
	u8  flip_h;
	u8  sheet;
	f32 x_scale, y_scale;
	f32 rotation;
} PORINGPart;

typedef struct {
	const char *name;
	u8 count;
	const PORINGPart *parts;
} PORINGPose;

static const PORINGPart poring_parts_BASE[] = {
	{ 352, 8, 56, 32, -8, -48, 56, 32, 28, 16, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 104, 72, -48, -64, 104, 72, 49, 63, 12, 0, 0, 1.0f, 1.0f, 0.0f },
};

static const PORINGPart poring_parts_PATTERN001[] = {
	{ 352, 8, 56, 32, -7, -45, 56, 32, 28, 16, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 104, 72, -48, -63, 104, 72, 49, 63, 12, 0, 0, 1.1f, 0.9f, 0.0f },
};

static const PORINGPart poring_parts_PATTERN002[] = {
	{ 352, 8, 56, 32, -10, -53, 56, 32, 28, 16, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 104, 72, -48, -64, 104, 72, 49, 63, 12, 0, 0, 0.95f, 1.1f, 0.0f },
};

static const PORINGPart poring_parts_PATTERN003[] = {
	{ 352, 8, 56, 32, -9, -59, 56, 32, 28, 16, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 104, 72, -48, -69, 104, 72, 49, 63, 12, 0, 0, 0.95f, 1.1f, 0.0f },
};

static const PORINGPart poring_parts_PATTERN004[] = {
	{ 352, 8, 56, 32, -8, -54, 56, 32, 28, 16, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 8, 8, 104, 72, -48, -69, 104, 72, 49, 63, 12, 0, 0, 1.0f, 1.0f, 0.0f },
};

#define PORING_POSE_COUNT 5
static const PORINGPose poring_poses[PORING_POSE_COUNT] = {
	{ "BASE", 2, poring_parts_BASE },
	{ "PATTERN001", 2, poring_parts_PATTERN001 },
	{ "PATTERN002", 2, poring_parts_PATTERN002 },
	{ "PATTERN003", 2, poring_parts_PATTERN003 },
	{ "PATTERN004", 2, poring_parts_PATTERN004 },
};

#endif
