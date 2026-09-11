#include "rbo_movie.h"

static RboMovieState s_state;
static int s_timer;

void rbo_movie_init(void)
{
	s_state = RBO_MOVIE_IDLE;
	s_timer = 0;
}

int rbo_movie_start_opening(void)
{
	/* PC: Data\Movie\opening.mpg — here: timed still / skip. */
	s_state = RBO_MOVIE_PLAYING;
	s_timer = 60 * 3; /* ~3s stand-in for INTRO */
	return 1;
}

void rbo_movie_update(int a_pressed, int start_pressed)
{
	if (s_state != RBO_MOVIE_PLAYING)
		return;
	if (a_pressed || start_pressed) {
		s_state = RBO_MOVIE_SKIPPED;
		return;
	}
	if (s_timer > 0)
		s_timer--;
	if (s_timer <= 0)
		s_state = RBO_MOVIE_FINISHED;
}

RboMovieState rbo_movie_state(void)
{
	return s_state;
}

void rbo_movie_stop(void)
{
	s_state = RBO_MOVIE_IDLE;
	s_timer = 0;
}
