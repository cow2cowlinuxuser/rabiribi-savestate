/* GameCube face-button icon linker — slices from textures/gc_pad_icons.png
 * (exported from 159596.png via tools/export-gc-pad-icons.ps1).
 */
#ifndef GC_PAD_ICONS_H
#define GC_PAD_ICONS_H

#include <gctypes.h>
#include <ogc/pad.h>
#include "ui_layouts.h"

#define GC_PAD_SHEET_W 256
#define GC_PAD_SHEET_H 128

typedef enum {
	RBO_PAD_ICON_A = 0,
	RBO_PAD_ICON_B,
	RBO_PAD_ICON_X,
	RBO_PAD_ICON_Y,
	RBO_PAD_ICON_A_SM,
	RBO_PAD_ICON_B_SM,
	RBO_PAD_ICON_X_SM,
	RBO_PAD_ICON_Y_SM,
	RBO_PAD_ICON_COUNT
} RboPadIcon;

/* sx,sy,sw,sh from atlas cells; dx/dy unused (caller supplies dest).
 * dw/dh = native pixel size for 1.0 scale. */
static const UiBlit k_pad_icons[RBO_PAD_ICON_COUNT] = {
	{ 12, 12, 40, 40, 0, 0, 40, 40 }, /* A */
	{ 76, 12, 40, 40, 0, 0, 40, 40 }, /* B */
	{ 146, 10, 28, 45, 0, 0, 28, 45 }, /* X */
	{ 204, 19, 39, 26, 0, 0, 39, 26 }, /* Y */
	{ 22, 86, 19, 19, 0, 0, 19, 19 }, /* A_SM */
	{ 86, 86, 20, 20, 0, 0, 20, 20 }, /* B_SM */
	{ 154, 85, 13, 22, 0, 0, 13, 22 }, /* X_SM */
	{ 213, 88, 22, 15, 0, 0, 22, 15 }, /* Y_SM */
};

/* Map PAD_BUTTON_* face bits to large icon. Returns -1 if unmapped. */
static inline int rbo_pad_icon_from_button(u16 pad_button)
{
	if (pad_button & PAD_BUTTON_A)
		return RBO_PAD_ICON_A;
	if (pad_button & PAD_BUTTON_B)
		return RBO_PAD_ICON_B;
	if (pad_button & PAD_BUTTON_X)
		return RBO_PAD_ICON_X;
	if (pad_button & PAD_BUTTON_Y)
		return RBO_PAD_ICON_Y;
	return -1;
}

static inline int rbo_pad_icon_small(int large_icon)
{
	switch (large_icon) {
	case RBO_PAD_ICON_A:
		return RBO_PAD_ICON_A_SM;
	case RBO_PAD_ICON_B:
		return RBO_PAD_ICON_B_SM;
	case RBO_PAD_ICON_X:
		return RBO_PAD_ICON_X_SM;
	case RBO_PAD_ICON_Y:
		return RBO_PAD_ICON_Y_SM;
	default:
		return large_icon;
	}
}

#endif /* GC_PAD_ICONS_H */
