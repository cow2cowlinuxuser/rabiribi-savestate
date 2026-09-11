#ifndef RBO_AUDIO_H
#define RBO_AUDIO_H

/*
 * DirectSound8 + wav / SE.pac drop-in (PC FUN_00419b30).
 *
 * BGM: Tremor OGG stream from sdc:/RBO/SOUND/bgm/ (*.ogg) to ASND.
 * SE:  small mono WAV resident from sdc:/RBO/SOUND/se/ (*.wav).
 */

#include <gctypes.h>

enum {
	RBO_SE_CONFIRM = 0,
	RBO_SE_CANCEL = 1,
	RBO_SE_COUNT
};

void rbo_audio_init(void);
void rbo_audio_shutdown(void);

/* FAT volume from sd_assets_mount(), e.g. "sdc". Required before play. */
void rbo_audio_set_vol(const char *vol);

/* Call often while the main thread is busy (FOB VM, SD load).
 * Wakes BGM decode LWP and yields so the worker can run. */
void rbo_audio_poll(void);

/* path relative to SD volume root, e.g. "RBO/SOUND/bgm/title.ogg".
 * Returns 0 on success, nonzero on miss/fail (soft — never fatal). */
int rbo_audio_play_bgm(const char *path, int loop);
void rbo_audio_stop_bgm(void);

int rbo_audio_play_se(u32 se_id);
void rbo_audio_stop_all_se(void);

#endif /* RBO_AUDIO_H */
