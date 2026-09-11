#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <asndlib.h>
#include <tremor/ivorbiscodec.h>
#include <tremor/ivorbisfile.h>

#include "rbo_audio.h"
#include "sd_assets.h"
#include "rbo_dvd.h"

#define RBO_AUD_BGM_VOICE 0
#define RBO_AUD_SE_VOICE0 1
#define RBO_AUD_SE_VOICES 3
#define RBO_AUD_VOL       180

/*
 * BGM Tremor decode runs on an LWP worker below main (~prio 63).
 * Input / scene / movement stay on main; audio only eats leftover CPU
 * (and time while main blocks on SD / VSync). ASND callback wakes the worker.
 */
#define RBO_BGM_BUF_BYTES      (128 * 1024)
#define RBO_BGM_DECODE_CHUNK   (8 * 1024)
#define RBO_BGM_SUBMIT_MIN     (16 * 1024)
#define RBO_BGM_THREAD_STACK   (16 * 1024)
#define RBO_BGM_THREAD_PRIO    48 /* < main (~63): input wins over decode */

typedef struct {
	void *pcm;
	u32 pcm_size;
	u32 rate;
	s32 format;
} RboWav;

static const char *s_vol;
static int s_ready;

/* ---- streamed OGG BGM ---- */
static FILE *s_bgm_fp;
static OggVorbis_File s_bgm_vf;
static int s_bgm_vf_open;
static char s_bgm_path[160];
static volatile int s_bgm_loop;
static u32 s_bgm_rate;
static s32 s_bgm_format;
static void *s_bgm_buf[2];
static u32 s_bgm_buf_cap;
static int s_bgm_fill_idx;
static u32 s_bgm_fill_got;
static volatile int s_bgm_active;
static volatile int s_bgm_eof;
static volatile int s_bgm_thread_run;

typedef struct {
	u32 disc_off;
	u32 size;
	u32 pos;
} DvdSrc;

static DvdSrc s_bgm_dvd;
static int s_bgm_dvd_open;

static lwp_t s_bgm_thread = LWP_THREAD_NULL;
static lwpq_t s_bgm_queue = LWP_TQUEUE_NULL;
static u8 s_bgm_stack[RBO_BGM_THREAD_STACK] ATTRIBUTE_ALIGN(8);

/* ---- resident SE (small WAV) ---- */
static RboWav s_se[RBO_SE_COUNT];
static int s_se_loaded[RBO_SE_COUNT];
static int s_se_rr;

static const char *const s_se_paths[RBO_SE_COUNT] = {
	"RBO/SOUND/se/sys_se01.wav",
	"RBO/SOUND/se/sys_se02.wav"
};

static u32 read_le16(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 read_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void wav_free(RboWav *w)
{
	if (!w)
		return;
	free(w->pcm);
	w->pcm = NULL;
	w->pcm_size = 0;
	w->rate = 0;
	w->format = VOICE_MONO_16BIT_LE;
}

static int wav_scan(FILE *fp, u32 *rate_out, s32 *fmt_out,
		    u32 *data_off_out, u32 *data_sz_out)
{
	u8 hdr[12];
	u8 chunk_hd[8];
	u8 fmt[16];
	u32 fmt_sz = 0, data_sz = 0, data_off = 0, pos;
	u32 audio_fmt, channels, rate, bits;
	long flen;

	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0)
		return -1;
	flen = ftell(fp);
	if (flen < 44 || fseek(fp, 0, SEEK_SET) != 0)
		return -1;
	if (fread(hdr, 1, 12, fp) != 12)
		return -1;
	if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
		return -1;

	pos = 12;
	while (pos + 8 <= (u32)flen) {
		u32 csz, next;

		if (fseek(fp, (long)pos, SEEK_SET) != 0)
			return -1;
		if (fread(chunk_hd, 1, 8, fp) != 8)
			return -1;
		csz = read_le32(chunk_hd + 4);
		next = pos + 8 + ((csz + 1u) & ~1u);

		if (memcmp(chunk_hd, "fmt ", 4) == 0) {
			fmt_sz = csz;
			if (fmt_sz < 16 || fread(fmt, 1, 16, fp) != 16)
				return -1;
		} else if (memcmp(chunk_hd, "data", 4) == 0) {
			data_sz = csz;
			data_off = pos + 8;
		}
		if (next <= pos)
			break;
		pos = next;
		if (fmt_sz && data_sz)
			break;
	}

	if (fmt_sz < 16 || data_sz == 0 || data_off == 0)
		return -1;

	audio_fmt = read_le16(fmt + 0);
	channels = read_le16(fmt + 2);
	rate = read_le32(fmt + 4);
	bits = read_le16(fmt + 14);
	if (audio_fmt != 1 || bits != 16 || rate == 0)
		return -1;
	if (channels != 1 && channels != 2)
		return -1;

	*rate_out = rate;
	*fmt_out = (channels == 2) ? VOICE_STEREO_16BIT_LE : VOICE_MONO_16BIT_LE;
	*data_off_out = data_off;
	*data_sz_out = data_sz;
	return 0;
}

static int wav_load_file_full(FILE *fp, RboWav *out)
{
	u32 rate, data_off, data_sz, alloc_sz;
	s32 format;
	void *pcm;

	if (wav_scan(fp, &rate, &format, &data_off, &data_sz) != 0)
		return -1;

	alloc_sz = (data_sz + 31u) & ~31u;
	pcm = memalign(32, alloc_sz);
	if (!pcm)
		return -1;
	if (fseek(fp, (long)data_off, SEEK_SET) != 0) {
		free(pcm);
		return -1;
	}
	if (fread(pcm, 1, data_sz, fp) != data_sz) {
		free(pcm);
		return -1;
	}
	if (alloc_sz > data_sz)
		memset((u8 *)pcm + data_sz, 0, alloc_sz - data_sz);
	DCFlushRange(pcm, alloc_sz);

	out->pcm = pcm;
	out->pcm_size = data_sz;
	out->rate = rate;
	out->format = format;
	return 0;
}

static int wav_scan_mem(const u8 *buf, u32 flen, u32 *rate_out, s32 *fmt_out,
			u32 *data_off_out, u32 *data_sz_out)
{
	u8 fmt[16];
	u32 fmt_sz = 0, data_sz = 0, data_off = 0, pos;
	u32 audio_fmt, channels, rate, bits;

	if (!buf || flen < 44)
		return -1;
	if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
		return -1;

	pos = 12;
	while (pos + 8 <= flen) {
		u32 csz, next;

		csz = read_le32(buf + pos + 4);
		next = pos + 8 + ((csz + 1u) & ~1u);
		if (memcmp(buf + pos, "fmt ", 4) == 0) {
			fmt_sz = csz;
			if (fmt_sz < 16 || pos + 8 + 16 > flen)
				return -1;
			memcpy(fmt, buf + pos + 8, 16);
		} else if (memcmp(buf + pos, "data", 4) == 0) {
			data_sz = csz;
			data_off = pos + 8;
		}
		if (next <= pos)
			break;
		pos = next;
		if (fmt_sz && data_sz)
			break;
	}

	if (fmt_sz < 16 || data_sz == 0 || data_off == 0)
		return -1;
	if (data_off + data_sz > flen)
		data_sz = flen - data_off;

	audio_fmt = read_le16(fmt + 0);
	channels = read_le16(fmt + 2);
	rate = read_le32(fmt + 4);
	bits = read_le16(fmt + 14);
	if (audio_fmt != 1 || bits != 16 || rate == 0)
		return -1;
	if (channels != 1 && channels != 2)
		return -1;

	*rate_out = rate;
	*fmt_out = (channels == 2) ? VOICE_STEREO_16BIT_LE : VOICE_MONO_16BIT_LE;
	*data_off_out = data_off;
	*data_sz_out = data_sz;
	return 0;
}

static int wav_load_mem(const u8 *buf, u32 flen, RboWav *out)
{
	u32 rate, data_off, data_sz, alloc_sz;
	s32 format;
	void *pcm;

	if (wav_scan_mem(buf, flen, &rate, &format, &data_off, &data_sz) != 0)
		return -1;

	alloc_sz = (data_sz + 31u) & ~31u;
	pcm = memalign(32, alloc_sz);
	if (!pcm)
		return -1;
	memcpy(pcm, buf + data_off, data_sz);
	if (alloc_sz > data_sz)
		memset((u8 *)pcm + data_sz, 0, alloc_sz - data_sz);
	DCFlushRange(pcm, alloc_sz);

	out->pcm = pcm;
	out->pcm_size = data_sz;
	out->rate = rate;
	out->format = format;
	return 0;
}

static size_t dvd_ov_read(void *ptr, size_t size, size_t nmemb, void *ds)
{
	DvdSrc *s = (DvdSrc *)ds;
	size_t want;
	u32 left;

	if (!s || !ptr || !size)
		return 0;
	want = size * nmemb;
	left = (s->pos < s->size) ? (s->size - s->pos) : 0;
	if (want > left)
		want = left;
	if (!want)
		return 0;
	if (rbo_dvd_read(s->disc_off + s->pos, ptr, (u32)want) != 0)
		return 0;
	s->pos += (u32)want;
	return want / size;
}

static int dvd_ov_seek(void *ds, ogg_int64_t offset, int whence)
{
	DvdSrc *s = (DvdSrc *)ds;
	ogg_int64_t p;

	if (!s)
		return -1;
	p = (ogg_int64_t)s->pos;
	if (whence == SEEK_SET)
		p = offset;
	else if (whence == SEEK_CUR)
		p += offset;
	else if (whence == SEEK_END)
		p = (ogg_int64_t)s->size + offset;
	else
		return -1;
	if (p < 0 || p > (ogg_int64_t)s->size)
		return -1;
	s->pos = (u32)p;
	return 0;
}

static int dvd_ov_close(void *ds)
{
	(void)ds;
	return 0;
}

static long dvd_ov_tell(void *ds)
{
	DvdSrc *s = (DvdSrc *)ds;

	return s ? (long)s->pos : -1;
}

static const ov_callbacks s_dvd_ov = {
	dvd_ov_read, dvd_ov_seek, dvd_ov_close, dvd_ov_tell
};

static FILE *fopen_sound_path(const char *path)
{
	char full[160];
	char alt[160];
	const char *rel[2];
	int n = 0, i;
	FILE *fp;

	if (!s_vol || !path)
		return NULL;
	if (strcmp(s_vol, SD_ASSETS_DVD_VOL) == 0)
		return NULL;

	rel[n++] = path;
	if (strncmp(path, "RBO/", 4) == 0) {
		snprintf(alt, sizeof(alt), "%s", path + 4);
		rel[n++] = alt;
	}

	for (i = 0; i < n; i++) {
		snprintf(full, sizeof(full), "%s:/%s", s_vol, rel[i]);
		fp = fopen(full, "rb");
		if (fp)
			return fp;
	}
	return NULL;
}

static int wav_load_path(const char *path, RboWav *out)
{
	FILE *fp;
	u8 *buf;
	u32 sz = 0;
	int rc;

	wav_free(out);
	fp = fopen_sound_path(path);
	if (fp) {
		if (wav_load_file_full(fp, out) != 0) {
			fclose(fp);
			wav_free(out);
			return -1;
		}
		fclose(fp);
		return 0;
	}
	if (!s_vol || strcmp(s_vol, SD_ASSETS_DVD_VOL) != 0)
		return -1;
	buf = sd_assets_load(s_vol, path, &sz);
	if (!buf && strncmp(path, "RBO/", 4) == 0)
		buf = sd_assets_load(s_vol, path + 4, &sz);
	if (!buf)
		return -1;
	rc = wav_load_mem(buf, sz, out);
	free(buf);
	if (rc != 0)
		wav_free(out);
	return rc;
}

static u32 bgm_frame_bytes(void)
{
	return (s_bgm_format == VOICE_STEREO_16BIT ||
		s_bgm_format == VOICE_STEREO_16BIT_LE) ? 4u : 2u;
}

static void bgm_wake(void)
{
	if (s_bgm_queue != LWP_TQUEUE_NULL)
		LWP_ThreadSignal(s_bgm_queue);
}

static void bgm_voice_cb(s32 voice)
{
	(void)voice;
	bgm_wake();
}

static void bgm_stop_thread(void)
{
	s_bgm_thread_run = 0;
	s_bgm_active = 0;
	bgm_wake();

	if (s_bgm_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(s_bgm_thread, NULL);
		s_bgm_thread = LWP_THREAD_NULL;
	}
	if (s_bgm_queue != LWP_TQUEUE_NULL) {
		LWP_CloseQueue(s_bgm_queue);
		s_bgm_queue = LWP_TQUEUE_NULL;
	}
}

static void bgm_close_stream(void)
{
	bgm_stop_thread();
	if (s_ready)
		ASND_StopVoice(RBO_AUD_BGM_VOICE);
	s_bgm_loop = 0;
	s_bgm_eof = 0;
	s_bgm_fill_got = 0;
	s_bgm_fill_idx = 0;
	if (s_bgm_vf_open) {
		ov_clear(&s_bgm_vf);
		s_bgm_vf_open = 0;
		s_bgm_fp = NULL; /* ov_clear closes FILE* (stdio callback only) */
	} else if (s_bgm_fp) {
		fclose(s_bgm_fp);
		s_bgm_fp = NULL;
	}
	s_bgm_dvd_open = 0;
	s_bgm_dvd.pos = 0;
	s_bgm_path[0] = 0;
}

/* Decode at most `budget` bytes into dst (Tremor). Worker-thread only. */
static u32 bgm_decode_bytes(void *dst, u32 budget)
{
	u32 align, got = 0;
	int section = 0;

	if (!s_bgm_vf_open || !dst || budget == 0)
		return 0;

	align = bgm_frame_bytes();
	budget = (budget / align) * align;
	if (budget < align)
		return 0;

	while (got + align <= budget) {
		long n = ov_read(&s_bgm_vf, (char *)dst + got, (int)(budget - got), &section);

		if (n > 0) {
			got += (u32)n;
			continue;
		}
		if (n == 0) {
			if (!s_bgm_loop) {
				s_bgm_eof = 1;
				break;
			}
			if (ov_pcm_seek(&s_bgm_vf, 0) != 0) {
				s_bgm_eof = 1;
				break;
			}
			continue;
		}
		if (n != OV_HOLE) {
			s_bgm_eof = 1;
			break;
		}
	}

	got = (got / align) * align;
	return got;
}

static int bgm_submit_fill(void)
{
	int idx = s_bgm_fill_idx & 1;
	u32 n = s_bgm_fill_got;
	u32 flush;
	s32 st;

	if (n == 0)
		return 0;
	if (ASND_TestPointer(RBO_AUD_BGM_VOICE, s_bgm_buf[idx]) == 1)
		return 0;

	flush = (n + 31u) & ~31u;
	if (flush > s_bgm_buf_cap)
		flush = s_bgm_buf_cap;
	if (flush > n)
		memset((u8 *)s_bgm_buf[idx] + n, 0, flush - n);
	DCFlushRange(s_bgm_buf[idx], flush);

	st = ASND_StatusVoice(RBO_AUD_BGM_VOICE);
	if (st == SND_UNUSED) {
		if (ASND_SetVoice(RBO_AUD_BGM_VOICE, s_bgm_format, (s32)s_bgm_rate,
				  0, s_bgm_buf[idx], (s32)n,
				  RBO_AUD_VOL, RBO_AUD_VOL, bgm_voice_cb) != SND_OK)
			return 0;
	} else {
		if (ASND_TestVoiceBufferReady(RBO_AUD_BGM_VOICE) != 1)
			return 0;
		if (ASND_AddVoice(RBO_AUD_BGM_VOICE, s_bgm_buf[idx], (s32)n) != SND_OK)
			return 0;
	}

	s_bgm_fill_idx ^= 1;
	s_bgm_fill_got = 0;
	return 1;
}

static u32 bgm_fill_buffer_full(int idx, u32 want)
{
	u32 got = 0;

	if (want > s_bgm_buf_cap)
		want = s_bgm_buf_cap;
	want = (want / bgm_frame_bytes()) * bgm_frame_bytes();

	while (got < want && !s_bgm_eof && s_bgm_thread_run) {
		u32 n = bgm_decode_bytes((u8 *)s_bgm_buf[idx] + got, want - got);

		if (n == 0)
			break;
		got += n;
	}
	return got;
}

static void *bgm_thread_main(void *arg)
{
	int first = 1;

	(void)arg;
	LWP_InitQueue(&s_bgm_queue);

	while (s_bgm_thread_run && s_bgm_vf_open) {
		int idx;
		u32 room, n, budget;
		s32 st;

		if (!s_bgm_buf[0] || !s_bgm_buf[1]) {
			LWP_ThreadSleep(s_bgm_queue);
			continue;
		}

		/* Bootstrap: fill both rings and start the voice. */
		if (first) {
			u32 n0, n1, flush;

			s_bgm_fill_got = 0;
			s_bgm_eof = 0;
			s_bgm_fill_idx = 0;
			ASND_StopVoice(RBO_AUD_BGM_VOICE);

			n0 = bgm_fill_buffer_full(0, s_bgm_buf_cap);
			if (n0 == 0)
				break;
			flush = (n0 + 31u) & ~31u;
			if (flush > s_bgm_buf_cap)
				flush = s_bgm_buf_cap;
			if (flush > n0)
				memset((u8 *)s_bgm_buf[0] + n0, 0, flush - n0);
			DCFlushRange(s_bgm_buf[0], flush);

			if (ASND_SetVoice(RBO_AUD_BGM_VOICE, s_bgm_format, (s32)s_bgm_rate,
					  0, s_bgm_buf[0], (s32)n0,
					  RBO_AUD_VOL, RBO_AUD_VOL, bgm_voice_cb) != SND_OK)
				break;

			n1 = bgm_fill_buffer_full(1, s_bgm_buf_cap);
			if (n1 == 0)
				break;
			flush = (n1 + 31u) & ~31u;
			if (flush > s_bgm_buf_cap)
				flush = s_bgm_buf_cap;
			if (flush > n1)
				memset((u8 *)s_bgm_buf[1] + n1, 0, flush - n1);
			DCFlushRange(s_bgm_buf[1], flush);
			ASND_AddVoice(RBO_AUD_BGM_VOICE, s_bgm_buf[1], (s32)n1);

			s_bgm_fill_idx = 0;
			s_bgm_fill_got = 0;
			s_bgm_active = 1;
			first = 0;
			continue;
		}

		idx = s_bgm_fill_idx & 1;
		if (ASND_TestPointer(RBO_AUD_BGM_VOICE, s_bgm_buf[idx]) == 1) {
			LWP_ThreadSleep(s_bgm_queue);
			continue;
		}

		if (s_bgm_fill_got < s_bgm_buf_cap && !s_bgm_eof) {
			room = s_bgm_buf_cap - s_bgm_fill_got;
			budget = RBO_BGM_DECODE_CHUNK;
			if (budget > room)
				budget = room;
			n = bgm_decode_bytes((u8 *)s_bgm_buf[idx] + s_bgm_fill_got, budget);
			s_bgm_fill_got += n;
			if (n == 0 && !s_bgm_eof) {
				usleep(500);
				continue;
			}
		}

		st = ASND_StatusVoice(RBO_AUD_BGM_VOICE);
		if (s_bgm_fill_got >= RBO_BGM_SUBMIT_MIN ||
		    (s_bgm_eof && s_bgm_fill_got >= bgm_frame_bytes()) ||
		    (st == SND_UNUSED && s_bgm_fill_got >= bgm_frame_bytes()) ||
		    (s_bgm_fill_got > 0 && s_bgm_fill_got >= s_bgm_buf_cap)) {
			if (!bgm_submit_fill()) {
				LWP_ThreadSleep(s_bgm_queue);
				continue;
			}
			continue;
		}

		if (s_bgm_eof && s_bgm_fill_got == 0 && st == SND_UNUSED) {
			/* Looped field BGM: seek and re-bootstrap instead of dying. */
			if (s_bgm_loop && ov_pcm_seek(&s_bgm_vf, 0) == 0) {
				s_bgm_eof = 0;
				first = 1;
				continue;
			}
			s_bgm_active = 0;
			break;
		}

		/* Waiting for more decode room or ASND to free a slot. */
		if (s_bgm_fill_got == 0 || st == SND_WORKING)
			LWP_ThreadSleep(s_bgm_queue);
		else
			usleep(200);
	}

	s_bgm_active = 0;
	s_bgm_thread_run = 0;
	return NULL;
}

static int bgm_ensure_bufs(void)
{
	int i;

	if (s_bgm_buf[0] && s_bgm_buf[1] && s_bgm_buf_cap == RBO_BGM_BUF_BYTES)
		return 0;

	for (i = 0; i < 2; i++) {
		free(s_bgm_buf[i]);
		s_bgm_buf[i] = memalign(32, RBO_BGM_BUF_BYTES);
		if (!s_bgm_buf[i]) {
			free(s_bgm_buf[0]);
			s_bgm_buf[0] = s_bgm_buf[1] = NULL;
			s_bgm_buf_cap = 0;
			return -1;
		}
		memset(s_bgm_buf[i], 0, RBO_BGM_BUF_BYTES);
		DCFlushRange(s_bgm_buf[i], RBO_BGM_BUF_BYTES);
	}
	s_bgm_buf_cap = RBO_BGM_BUF_BYTES;
	return 0;
}

static int bgm_start_stream(void)
{
	if (!s_bgm_vf_open || bgm_ensure_bufs() != 0)
		return -1;

	bgm_stop_thread();
	ASND_StopVoice(RBO_AUD_BGM_VOICE);

	s_bgm_eof = 0;
	s_bgm_fill_got = 0;
	s_bgm_fill_idx = 0;
	s_bgm_active = 0;
	s_bgm_thread_run = 1;
	s_bgm_queue = LWP_TQUEUE_NULL;

	if (LWP_CreateThread(&s_bgm_thread, bgm_thread_main, NULL,
			     s_bgm_stack, RBO_BGM_THREAD_STACK,
			     RBO_BGM_THREAD_PRIO) < 0) {
		s_bgm_thread = LWP_THREAD_NULL;
		s_bgm_thread_run = 0;
		return -1;
	}
	return 0;
}

void rbo_audio_init(void)
{
	s_vol = NULL;
	s_ready = 0;
	s_bgm_fp = NULL;
	s_bgm_vf_open = 0;
	memset(&s_bgm_vf, 0, sizeof(s_bgm_vf));
	s_bgm_path[0] = 0;
	s_bgm_loop = 0;
	s_bgm_active = 0;
	s_bgm_eof = 0;
	s_bgm_fill_got = 0;
	s_bgm_fill_idx = 0;
	s_bgm_thread_run = 0;
	s_bgm_thread = LWP_THREAD_NULL;
	s_bgm_queue = LWP_TQUEUE_NULL;
	s_bgm_buf[0] = s_bgm_buf[1] = NULL;
	s_bgm_buf_cap = 0;
	memset(s_se, 0, sizeof(s_se));
	memset(s_se_loaded, 0, sizeof(s_se_loaded));
	s_se_rr = 0;

	ASND_Init();
	ASND_Pause(0);
	s_ready = 1;
}

void rbo_audio_shutdown(void)
{
	int i;

	if (!s_ready)
		return;

	rbo_audio_stop_bgm();
	rbo_audio_stop_all_se();
	for (i = 0; i < RBO_SE_COUNT; i++) {
		wav_free(&s_se[i]);
		s_se_loaded[i] = 0;
	}
	free(s_bgm_buf[0]);
	free(s_bgm_buf[1]);
	s_bgm_buf[0] = s_bgm_buf[1] = NULL;
	s_bgm_buf_cap = 0;
	ASND_End();
	s_ready = 0;
	s_vol = NULL;
}

void rbo_audio_set_vol(const char *vol)
{
	s_vol = vol;
}

void rbo_audio_poll(void)
{
	/* Nudge the decode worker and yield so it can run (GC LWP is cooperative).
	 * FULL FOB VM can otherwise starve BGM until the track ends and dies. */
	if (!s_ready)
		return;

	if (s_bgm_loop && s_bgm_vf_open && s_bgm_path[0] &&
	    (!s_bgm_thread_run || s_bgm_thread == LWP_THREAD_NULL)) {
		s_bgm_eof = 0;
		if (ov_pcm_seek(&s_bgm_vf, 0) == 0)
			(void)bgm_start_stream();
	}

	if (s_bgm_thread_run)
		bgm_wake();
	LWP_YieldThread();
}

int rbo_audio_play_bgm(const char *path, int loop)
{
	vorbis_info *vi;

	if (!s_ready || !path)
		return -1;

	if (s_bgm_vf_open && s_bgm_path[0] && strcmp(s_bgm_path, path) == 0) {
		s_bgm_loop = loop ? 1 : 0;
		if (!s_bgm_thread_run || s_bgm_thread == LWP_THREAD_NULL) {
			s_bgm_eof = 0;
			if (ov_pcm_seek(&s_bgm_vf, 0) != 0)
				return -1;
			return bgm_start_stream();
		}
		bgm_wake();
		return 0;
	}

	rbo_audio_stop_bgm();

	s_bgm_fp = fopen_sound_path(path);
	memset(&s_bgm_vf, 0, sizeof(s_bgm_vf));
	if (s_bgm_fp) {
		if (ov_open(s_bgm_fp, &s_bgm_vf, NULL, 0) < 0) {
			fclose(s_bgm_fp);
			s_bgm_fp = NULL;
			return -1;
		}
		s_bgm_fp = NULL;
	} else if (s_vol && strcmp(s_vol, SD_ASSETS_DVD_VOL) == 0 &&
		   rbo_dvd_lookup(path, &s_bgm_dvd.disc_off, &s_bgm_dvd.size) == 0) {
		s_bgm_dvd.pos = 0;
		s_bgm_dvd_open = 1;
		if (ov_open_callbacks(&s_bgm_dvd, &s_bgm_vf, NULL, 0, s_dvd_ov) < 0) {
			s_bgm_dvd_open = 0;
			return -1;
		}
	} else {
		return -1;
	}
	s_bgm_vf_open = 1;

	vi = ov_info(&s_bgm_vf, -1);
	if (!vi || vi->rate <= 0 || (vi->channels != 1 && vi->channels != 2)) {
		bgm_close_stream();
		return -1;
	}

	s_bgm_rate = (u32)vi->rate;
	/* Tremor on PPC emits host-endian s16; match stock oggplayer (BE voices). */
	s_bgm_format = (vi->channels == 2) ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
	snprintf(s_bgm_path, sizeof(s_bgm_path), "%s", path);
	s_bgm_loop = loop ? 1 : 0;

	if (bgm_start_stream() != 0) {
		bgm_close_stream();
		return -1;
	}
	return 0;
}

void rbo_audio_stop_bgm(void)
{
	bgm_close_stream();
}

static int se_ensure_loaded(u32 se_id)
{
	if (se_id >= RBO_SE_COUNT)
		return -1;
	if (s_se_loaded[se_id])
		return 0;
	if (wav_load_path(s_se_paths[se_id], &s_se[se_id]) != 0)
		return -1;
	s_se_loaded[se_id] = 1;
	return 0;
}

int rbo_audio_play_se(u32 se_id)
{
	s32 voice;
	int i;
	RboWav *w;

	if (!s_ready)
		return -1;
	if (se_ensure_loaded(se_id) != 0)
		return -1;

	w = &s_se[se_id];
	if (!w->pcm)
		return -1;

	voice = -1;
	for (i = 0; i < RBO_AUD_SE_VOICES; i++) {
		s32 v = RBO_AUD_SE_VOICE0 + ((s_se_rr + i) % RBO_AUD_SE_VOICES);

		if (ASND_StatusVoice(v) == SND_UNUSED) {
			voice = v;
			break;
		}
	}
	if (voice < 0) {
		voice = RBO_AUD_SE_VOICE0 + (s_se_rr % RBO_AUD_SE_VOICES);
		ASND_StopVoice(voice);
	}
	s_se_rr = (s_se_rr + 1) % RBO_AUD_SE_VOICES;

	ASND_SetVoice(voice, w->format, (s32)w->rate, 0, w->pcm, (s32)w->pcm_size,
		      RBO_AUD_VOL, RBO_AUD_VOL, NULL);
	return 0;
}

void rbo_audio_stop_all_se(void)
{
	int i;

	if (!s_ready)
		return;
	for (i = 0; i < RBO_AUD_SE_VOICES; i++)
		ASND_StopVoice(RBO_AUD_SE_VOICE0 + i);
}
