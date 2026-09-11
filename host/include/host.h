#ifndef RBO_HOST_H
#define RBO_HOST_H

#include "host_win.h"

void host_init(void);
void host_shutdown(void);

/* GC VI/GX objects the DD Flip path presents into. */
void host_bind_video(void *rmode, void *xfb0, void *xfb1);

void host_log(const char *fmt, ...);
void host_trap(const char *api);
int host_trapped(void);
const char *host_trap_name(void);
const char *host_log_line(int i);
int host_log_count(void);

void host_log_draw(void);
void host_present_efb(void);

/* Overlay + VSync + START. Safe during leave/enter before the next Flip. */
void host_overlay_pump(void);

/* DD Flip has presented at least once. IMG decode must not overlay_pump
 * after this: a second GX_DrawDone on a live D3D frame wedges (FPS 0). */
void host_note_flip(void);
int host_flip_live(void);

/* Restrict CreateFile to this FAT volume (sdc on SD2SP2). */
void host_set_sd_vol(const char *vol);
const char *host_sd_vol(void);

/* Arena leftover + newlib heap used/free, in KB. Safe to call anytime. */
void host_mem1_log(const char *tag);

/* Poll pad and honor START → Swiss. Safe during long IMG decodes. */
void host_pump_escape(void);

/* Bottom-left badge. halt = currently inside this FUN (frozen if it
 * never returns). spin = that FUN returned and the WinMain loop is live. */
void host_halt(const char *fun);
void host_spin(const char *fun);

/* Until FUN_00440690 is linked: exercise DD/DS/DI and Flip. */
int host_winmain(void);

DWORD host_tick_ms(void);

void *host_img_load_path(const char *path);
void *host_tpl_load(const char *tpl_file, int tex_id);
void *host_load_atlas(const char *img_pc, const char *tpl_file, int tex_id);
void *host_load_title_tex(void);
void *host_load_system_tex(void);
void *host_load_lobby_tex(void);
void *host_load_stsel_tex(void);
void *host_load_stsel_chip(void);

void title_standin_leave(void);
void stsel_leave(void);
void *stsel_atlas(void);
void *stsel_chip_tex(void);

/* Mode 0 is title stand-in until stsel confirms. Then it is GameMain. */
void host_set_play(int play);
int host_is_play(void);

/* Proto FOB VM on play enter (FUN_00434160 / 250d0 / 34220 analog). */
void play_fob_leave(void);
void play_fob_tick(void);

/* STAGE01.TPL plates on the walkscape dests. Free on leave play. */
void play_bg_leave(void);

/* Copy n bytes of initialized PE .data at VA into dst. 0 if out of range. */
unsigned host_pe_copy(void *dst, unsigned va, unsigned n);

#endif
