/* Auto-generated CHON CHON walk subset - do not edit */
#ifndef CHON_CHON_ANIMS_H
#define CHON_CHON_ANIMS_H
#include <gctypes.h>

#define CHON_CHON_SHEET_W 512
#define CHON_CHON_SHEET_H 512
#define CHON_CHON_SHEET_COUNT 1

typedef struct {
	s16 src_x, src_y, src_w, src_h;
	s16 dest_x, dest_y, dest_w, dest_h;
	s16 origin_x, origin_y;
	s8  layer;
	u8  flip_h;
	u8  sheet;
	f32 x_scale, y_scale;
	f32 rotation;
} CHON_CHONPart;

typedef struct {
	const char *name;
	u8 count;
	const CHON_CHONPart *parts;
} CHON_CHONPose;

static const CHON_CHONPart chon_chon_parts_BASE[] = {
	{ 104, 56, 48, 48, -26, -136, 48, 48, 22, 40, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 112, 112, 40, 40, -24, -102, 40, 40, 22, 8, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 128, 0, 80, 56, -44, -118, 80, 56, 40, 28, 20, 0, 0, 1.0f, 1.0f, 0.0f },
};

static const CHON_CHONPart chon_chon_parts_PATTERN001[] = {
	{ 104, 56, 48, 48, -26, -135, 48, 48, 22, 40, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 112, 112, 40, 40, -24, -101, 40, 40, 22, 8, 15, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 128, 0, 80, 56, -44, -117, 80, 56, 40, 28, 20, 0, 0, 1.0f, 1.0f, 0.0f },
};

static const CHON_CHONPart chon_chon_parts_PATTERN002[] = {
	{ 56, 56, 48, 48, 43, -86, 48, 48, 12, 40, 10, 0, 0, 1.0f, 1.0f, 358.848f },
	{ 80, 8, 40, 48, 14, -85, 40, 48, 33, 22, 10, 0, 0, 1.0f, 1.0f, 49.608f },
	{ 64, 112, 32, 40, 31, -60, 32, 40, 16, 6, 15, 0, 0, 1.0f, 1.0f, 35.352f },
};

static const CHON_CHONPart chon_chon_parts_PATTERN003[] = {
	{ 56, 56, 48, 48, -11, -134, 48, 48, 12, 40, 10, 0, 0, 1.0f, 1.0f, 0.0f },
	{ 80, 8, 40, 48, -48, -123, 40, 48, 33, 22, 10, 0, 0, 1.0f, 1.0f, 20.16f },
	{ 64, 112, 32, 40, -16, -99, 32, 40, 16, 6, 15, 0, 0, 1.0f, 1.0f, 52.596f },
};

static const CHON_CHONPart chon_chon_parts_PATTERN004[] = {
	{ 56, 56, 48, 48, 46, -86, 48, 48, 12, 40, 10, 0, 0, 1.0f, 1.0f, 358.848f },
	{ 80, 8, 40, 48, 17, -86, 40, 48, 33, 22, 10, 0, 0, 1.0f, 1.0f, 49.608f },
	{ 64, 112, 32, 40, 34, -60, 32, 40, 16, 6, 15, 0, 0, 1.0f, 1.0f, 51.66f },
};

#define CHON_CHON_POSE_COUNT 5
static const CHON_CHONPose chon_chon_poses[CHON_CHON_POSE_COUNT] = {
	{ "BASE", 3, chon_chon_parts_BASE },
	{ "PATTERN001", 3, chon_chon_parts_PATTERN001 },
	{ "PATTERN002", 3, chon_chon_parts_PATTERN002 },
	{ "PATTERN003", 3, chon_chon_parts_PATTERN003 },
	{ "PATTERN004", 3, chon_chon_parts_PATTERN004 },
};

#endif
