#ifndef RBO_MOVIE_H
#define RBO_MOVIE_H

/*
 * DirectShow opening.mpg drop-in (PC FUN_0043e920 / FUN_00453f70 FilterGraph).
 * No MPEG decode yet — still frame + skip on A/Start.
 */

#include <gctypes.h>

typedef enum {
	RBO_MOVIE_IDLE = 0,
	RBO_MOVIE_PLAYING,
	RBO_MOVIE_FINISHED,
	RBO_MOVIE_SKIPPED
} RboMovieState;

void rbo_movie_init(void);

/* Begin stub playback (uses optional still tex id / timer). */
int rbo_movie_start_opening(void);

void rbo_movie_update(int a_pressed, int start_pressed);
RboMovieState rbo_movie_state(void);
void rbo_movie_stop(void);

#endif /* RBO_MOVIE_H */
