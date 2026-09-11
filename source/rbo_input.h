#ifndef RBO_INPUT_H
#define RBO_INPUT_H

/*
 * DirectInput8 drop-in (PC FUN_00418850).
 * GC: PAD wrapper. Maps A/B/X/Y/Start/D-Pad + stick.
 * Multi-pad for lobby join (PC KeyConfig / "Not enough pads").
 *
 * Poll once per frame via rbo_input_poll(). Do not also PAD_Read elsewhere
 * in the same frame (except escape reset line).
 */

#include <gccore.h>

typedef struct {
	u16 held;
	u16 pressed;  /* rising edge */
	u16 released; /* falling edge */
	s8 stick_x;
	s8 stick_y;
	int connected;
} RboPad;

void rbo_input_init(void);

/* Call once per frame before reading. */
void rbo_input_poll(void);

const RboPad *rbo_input_pad(int chan);

/* Convenience on chan 0. */
int rbo_input_pressed(u16 mask);
int rbo_input_held(u16 mask);

/* Any connected pad: rising edge / held. */
int rbo_input_any_pressed(u16 mask);
int rbo_input_any_held(u16 mask);

/* Lobby-style: any connected pad holding A. */
int rbo_input_any_holding_a(void);

/* Count of connected pads currently holding A (party size stub). */
int rbo_input_count_holding_a(void);

/* After a mode change: ignore A/B/X until they have all been released. */
void rbo_input_disarm_face(void);
int rbo_input_face_pressed(u16 mask);

#endif /* RBO_INPUT_H */
