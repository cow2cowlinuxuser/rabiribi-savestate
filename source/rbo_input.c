#include <string.h>

#include "rbo_input.h"

static RboPad s_pads[PAD_CHANMAX];
static u16 s_prev[PAD_CHANMAX];
static int s_face_arm;

void rbo_input_init(void)
{
	memset(s_pads, 0, sizeof(s_pads));
	memset(s_prev, 0, sizeof(s_prev));
	s_face_arm = 1;
}

void rbo_input_poll(void)
{
	PADStatus st[PAD_CHANMAX];
	int i;

	PAD_Read(st);
	for (i = 0; i < PAD_CHANMAX; i++) {
		u16 now;

		if (st[i].err != PAD_ERR_NONE) {
			/* Keep last buttons. A one-frame PAD_ERR_* would
			 * otherwise look like a release+press. */
			continue;
		}
		now = st[i].button;
		s_pads[i].connected = 1;
		s_pads[i].stick_x = st[i].stickX;
		s_pads[i].stick_y = st[i].stickY;
		s_pads[i].held = now;
		s_pads[i].pressed = (u16)(now & ~s_prev[i]);
		s_pads[i].released = (u16)(s_prev[i] & ~now);
		s_prev[i] = now;
	}
}

const RboPad *rbo_input_pad(int chan)
{
	if (chan < 0 || chan >= PAD_CHANMAX)
		return &s_pads[0];
	return &s_pads[chan];
}

int rbo_input_pressed(u16 mask)
{
	return (s_pads[0].pressed & mask) != 0;
}

int rbo_input_held(u16 mask)
{
	return (s_pads[0].held & mask) != 0;
}

int rbo_input_any_pressed(u16 mask)
{
	int i;

	for (i = 0; i < PAD_CHANMAX; i++) {
		if (s_pads[i].connected && (s_pads[i].pressed & mask))
			return 1;
	}
	return 0;
}

int rbo_input_any_held(u16 mask)
{
	int i;

	for (i = 0; i < PAD_CHANMAX; i++) {
		if (s_pads[i].connected && (s_pads[i].held & mask))
			return 1;
	}
	return 0;
}

int rbo_input_any_holding_a(void)
{
	return rbo_input_any_held(PAD_BUTTON_A);
}

int rbo_input_count_holding_a(void)
{
	int i, n = 0;

	for (i = 0; i < PAD_CHANMAX; i++) {
		if (s_pads[i].connected && (s_pads[i].held & PAD_BUTTON_A))
			n++;
	}
	return n;
}

void rbo_input_disarm_face(void)
{
	s_face_arm = 0;
}

int rbo_input_face_pressed(u16 mask)
{
	u16 face = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X;

	if (!rbo_input_any_held(face))
		s_face_arm = 1;
	return s_face_arm && rbo_input_any_pressed(mask);
}
