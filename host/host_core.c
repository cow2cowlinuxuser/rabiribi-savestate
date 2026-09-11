#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "host.h"
#include "rbo_gx.h"
#include "rbo_input.h"
#include "escape.h"

#define LOG_LINES 16
#define LOG_COLS 48

static char s_lines[LOG_LINES][LOG_COLS];
static int s_n;
static int s_trapped;
static char s_trap[80];
static void *s_rmode;
static void *s_xfb[2];

static int s_shown; /* index of XFB currently on the TV */

static unsigned s_fps_shown;
static unsigned s_fps_frames;
static DWORD s_fps_t0;
static int s_flip_live;
static char s_run_fun[20];
static int s_run_halt;
static unsigned s_spin_n;
static int s_play;

void host_bind_video(void *rmode, void *xfb0, void *xfb1)
{
	s_rmode = rmode;
	s_xfb[0] = xfb0;
	s_xfb[1] = xfb1;
	s_shown = 0;
}

void host_log(const char *fmt, ...)
{
	va_list ap;
	char buf[LOG_COLS];
	int i;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (s_n < LOG_LINES) {
		snprintf(s_lines[s_n], LOG_COLS, "%s", buf);
		s_n++;
		return;
	}
	for (i = 1; i < LOG_LINES; i++)
		memcpy(s_lines[i - 1], s_lines[i], LOG_COLS);
	snprintf(s_lines[LOG_LINES - 1], LOG_COLS, "%s", buf);
}

void host_trap(const char *api)
{
	if (!s_trapped) {
		snprintf(s_trap, sizeof(s_trap), "%s", api ? api : "?");
		s_trapped = 1;
	}
	host_log("TRAP %s", api ? api : "?");
}

int host_trapped(void)
{
	return s_trapped;
}

const char *host_trap_name(void)
{
	return s_trap;
}

const char *host_log_line(int i)
{
	if (i < 0 || i >= s_n)
		return "";
	return s_lines[i];
}

int host_log_count(void)
{
	return s_n;
}

static void quad(f32 x, f32 y, f32 w, f32 h)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x, y);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x + w, y);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x + w, y + h);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x, y + h);
		GX_TexCoord2f32(0, 0);
	GX_End();
}

/* 5x7 glyphs for 0-9 A-Z space .:/+- */
static const u8 kfont[][7] = {
	{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
	{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
	{0x0e,0x11,0x01,0x06,0x08,0x10,0x1f},
	{0x0e,0x11,0x01,0x06,0x01,0x11,0x0e},
	{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
	{0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
	{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},
	{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
	{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
	{0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
	{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
	{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
	{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
	{0x1c,0x12,0x11,0x11,0x11,0x12,0x1c},
	{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
	{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
	{0x0e,0x11,0x10,0x17,0x11,0x11,0x0e},
	{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
	{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
	{0x01,0x01,0x01,0x01,0x11,0x11,0x0e},
	{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
	{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
	{0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
	{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
	{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
	{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
	{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
	{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
	{0x0e,0x11,0x10,0x0e,0x01,0x11,0x0e},
	{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
	{0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
	{0x11,0x11,0x11,0x15,0x15,0x1b,0x11},
	{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
	{0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
	{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
};

static int glyph_index(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'Z')
		return 10 + (c - 'A');
	if (c >= 'a' && c <= 'z')
		return 10 + (c - 'a');
	return -1;
}

static void draw_char(f32 x, f32 y, char c, f32 s)
{
	int gi = glyph_index(c);
	int row, col;

	if (gi < 0)
		return;
	for (row = 0; row < 7; row++) {
		u8 bits = kfont[gi][row];
		for (col = 0; col < 5; col++) {
			if (bits & (0x10 >> col))
				quad(x + col * s, y + row * s, s, s);
		}
	}
}

static void draw_str(f32 x, f32 y, const char *s, f32 sc)
{
	f32 cx = x;

	for (; *s; s++) {
		if (*s == ' ') {
			cx += 6.0f * sc;
			continue;
		}
		draw_char(cx, y, *s, sc);
		cx += 6.0f * sc;
	}
}

static void fps_tick(void)
{
	DWORD t = host_tick_ms();
	unsigned dt;

	s_fps_frames++;
	if (!s_fps_t0) {
		s_fps_t0 = t;
		return;
	}
	dt = (unsigned)(t - s_fps_t0);
	if (dt >= 1000) {
		s_fps_shown = s_fps_frames * 1000u / dt;
		s_fps_frames = 0;
		s_fps_t0 = t;
	}
}

static void badge_draw(f32 x, f32 y, const char *s, GXColor fg)
{
	f32 sc = 2.0f;
	unsigned n = (unsigned)strlen(s);
	f32 w = (f32)n * 6.0f * sc;

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 255});
	quad(x - 6.0f, y - 4.0f, w + 12.0f, 7.0f * sc + 8.0f);
	GX_SetChanMatColor(GX_COLOR0A0, fg);
	draw_str(x, y, s, sc);
}

static void fps_draw(void)
{
	char buf[12];
	f32 sc = 2.0f;
	f32 w;
	f32 x;
	f32 y;
	unsigned n;

	snprintf(buf, sizeof(buf), "FPS %u", s_fps_shown);
	n = (unsigned)strlen(buf);
	w = (f32)n * 6.0f * sc;
	x = 640.0f - 20.0f - w;
	y = 480.0f - 20.0f - 7.0f * sc;
	badge_draw(x, y, buf, (GXColor){80, 255, 120, 255});
}

static void run_draw(void)
{
	char buf[28];
	f32 y = 480.0f - 20.0f - 7.0f * 2.0f;

	if (s_run_halt && s_run_fun[0])
		snprintf(buf, sizeof(buf), "HALT %s", s_run_fun);
	else if (s_run_fun[0])
		snprintf(buf, sizeof(buf), "SPIN %s %u", s_run_fun, s_spin_n);
	else
		snprintf(buf, sizeof(buf), "SPIN %u", s_spin_n);
	badge_draw(12.0f, y, buf, s_run_halt
					 ? (GXColor){255, 180, 40, 255}
					 : (GXColor){80, 200, 255, 255});
}

void host_halt(const char *fun)
{
	snprintf(s_run_fun, sizeof(s_run_fun), "%s", fun ? fun : "?");
	s_run_halt = 1;
}

void host_spin(const char *fun)
{
	snprintf(s_run_fun, sizeof(s_run_fun), "%s", fun ? fun : "loop");
	s_run_halt = 0;
	s_spin_n++;
}

void host_set_play(int play)
{
	s_play = play ? 1 : 0;
}

int host_is_play(void)
{
	return s_play;
}

void host_log_draw(void)
{
	int i;

	fps_tick();
	GX_SetNumChans(1);
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 240, 255});
	rbo_gx_set_ui_ortho();
	for (i = 0; i < s_n; i++)
		draw_str(12.0f, 12.0f + i * 14.0f, s_lines[i], 1.5f);
	if (s_trapped) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 64, 64, 255});
		draw_str(12.0f, 430.0f, s_trap, 2.0f);
	}
	fps_draw();
	run_draw();
}

void host_init(void)
{
	s_n = 0;
	s_trapped = 0;
	s_trap[0] = 0;
	host_log("RBO EX3 HOST");
}

void host_shutdown(void)
{
}

void host_present_efb(void)
{
	static int primed;
	void *back;

	if (!s_xfb[0])
		return;
	back = s_xfb[s_shown ^ 1];
	if (!back)
		back = s_xfb[0];
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_DrawDone();
	if (!primed) {
		/* First CopyDisp dumps uninitialized EFB. Do that into the
		 * hidden XFB so the CPU diagnostic bands stay on screen. */
		GX_CopyDisp(back, GX_TRUE);
		GX_DrawDone();
		primed = 1;
	}
	GX_CopyDisp(back, GX_TRUE);
	VIDEO_SetNextFramebuffer(back);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	s_shown ^= 1;
	(void)s_rmode;
}

DWORD host_tick_ms(void)
{
	return (DWORD)(ticks_to_millisecs(gettime()));
}

void host_mem1_log(const char *tag)
{
	struct mallinfo m = mallinfo();
	u32 arena = SYS_GetArena1Size();

	host_log("MEM1 %s a%u u%u f%u", tag ? tag : "x",
		 (unsigned)(arena / 1024u),
		 (unsigned)(m.uordblks / 1024u),
		 (unsigned)(m.fordblks / 1024u));
}

void host_pump_escape(void)
{
	rbo_input_poll();
	if (rbo_input_any_held(PAD_BUTTON_START) && escape_available())
		escape_now();
}

void host_overlay_pump(void)
{
	rbo_input_poll();
	host_log_draw();
	host_present_efb();
	if (rbo_input_any_held(PAD_BUTTON_START) && escape_available())
		escape_now();
}

void host_note_flip(void)
{
	s_flip_live = 1;
}

int host_flip_live(void)
{
	return s_flip_live;
}
