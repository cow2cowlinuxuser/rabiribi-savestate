#include "rbo_hud.h"

#include <stdio.h>
#include <gccore.h>

#include "rbo_gx.h"
#include "ui_layouts.h"

static void hud_solid(f32 x, f32 y, f32 w, f32 h)
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

static void hud_blit(GXTexObj *tex, const UiBlit *b)
{
	if (!tex || !b)
		return;
	rbo_gx_blit_src(tex, b->dx, b->dy, b->dw, b->dh,
			b->sx, b->sy, b->sw, b->sh,
			(f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
}

static void hud_blit_at(GXTexObj *tex, const UiBlit *src,
			f32 dx, f32 dy, f32 dw, f32 dh)
{
	if (!tex || !src)
		return;
	rbo_gx_blit_src(tex, dx, dy, dw, dh,
			src->sx, src->sy, src->sw, src->sh,
			(f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
}

static void draw_bar_pair(GXTexObj *tex, f32 slot_x, f32 slot_y,
			  int hp, int hp_max, int sp, int sp_max)
{
	UiBlit fill;
	f32 hp_w, sp_w;

	if (hp_max < 1)
		hp_max = 1;
	if (sp_max < 1)
		sp_max = 1;
	if (hp < 0)
		hp = 0;
	if (sp < 0)
		sp = 0;
	hp_w = k_sys_hp_fill.dw * (f32)hp / (f32)hp_max;
	sp_w = k_sys_sp_fill.dw * (f32)sp / (f32)sp_max;

	fill = k_sys_bar_empty;
	hud_blit_at(tex, &fill, slot_x + k_sys_hp_fill.dx,
		    slot_y + k_sys_hp_fill.dy, fill.dw, fill.dh);
	hud_blit_at(tex, &fill, slot_x + k_sys_sp_fill.dx,
		    slot_y + k_sys_sp_fill.dy, fill.dw, fill.dh);

	fill = k_sys_hp_fill;
	fill.dw = hp_w;
	fill.sw = hp_w;
	hud_blit_at(tex, &fill, slot_x + fill.dx, slot_y + fill.dy,
		    fill.dw, fill.dh);
	fill = k_sys_sp_fill;
	fill.dw = sp_w;
	fill.sw = sp_w;
	hud_blit_at(tex, &fill, slot_x + fill.dx, slot_y + fill.dy,
		    fill.dw, fill.dh);
}

static void draw_party_slots(const RboHudView *v, GXTexObj *tex)
{
	int i;
	UiBlit frame;

	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	for (i = 0; i < HUD_PARTY_SLOTS; i++) {
		f32 sx = k_sys_slot_x[i];

		if (v->slot[i].occupied) {
			frame = k_sys_hp_frame;
			hud_blit_at(tex, &frame, sx, HUD_SLOT_Y,
				    HUD_SLOT_W, HUD_SLOT_H);
			draw_bar_pair(tex, sx, HUD_SLOT_Y,
				      v->slot[i].hp, v->slot[i].hp_max,
				      v->slot[i].sp, v->slot[i].sp_max);
		} else {
			frame = k_sys_hp_frame_empty;
			hud_blit_at(tex, &frame, sx, HUD_SLOT_Y,
				    HUD_SLOT_W, HUD_SLOT_H);
		}
	}
}

static void draw_follow_bars(const RboHudView *v, GXTexObj *tex)
{
	const RboHudSlot *s = &v->slot[1];
	f32 x, y;

	if (!s->occupied)
		s = &v->slot[0];
	if (!s->occupied)
		return;

	x = v->novice_sx + HUD_FOLLOW_HP_OX;
	y = v->novice_sy + HUD_FOLLOW_HP_OY;
	draw_bar_pair(tex, x - k_sys_hp_fill.dx, y - k_sys_hp_fill.dy,
		      s->hp, s->hp_max, s->sp, s->sp_max);
}

static void draw_radar(const RboHudView *v, GXTexObj *tex)
{
	int i;
	f32 x0, x1, span, bx, by, bw, bh;

	hud_blit(tex, &k_sys_radar);

	x0 = v->cam_x - 400.0f;
	x1 = v->cam_x + (f32)RBO_SCREEN_W + 400.0f;
	if (x0 < v->lock_lo)
		x0 = v->lock_lo;
	if (x1 > v->lock_hi)
		x1 = v->lock_hi;
	span = x1 - x0;
	if (span < 1.0f)
		span = 1.0f;

	bx = k_sys_radar.dx + HUD_RADAR_PAD;
	by = k_sys_radar.dy + k_sys_radar.dh * 0.5f - 2.0f;
	bw = k_sys_radar.dw - HUD_RADAR_PAD * 2.0f;
	bh = 4.0f;

	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	for (i = 0; i < v->n_blips; i++) {
		f32 t, px;
		GXColor c;

		t = (v->blips[i].world_x - x0) / span;
		if (t < 0.0f)
			t = 0.0f;
		if (t > 1.0f)
			t = 1.0f;
		px = bx + t * bw;
		switch (v->blips[i].kind) {
		case RBO_HUD_BLIP_PLAYER:
			c = (GXColor){80, 220, 255, 255};
			break;
		case RBO_HUD_BLIP_NPC:
			c = (GXColor){180, 120, 255, 255};
			break;
		default:
			c = (GXColor){80, 220, 80, 255};
			break;
		}
		GX_SetChanMatColor(GX_COLOR0A0, c);
		hud_solid(px - 2.0f, by, 4.0f, bh);
	}

	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

static void draw_top_text(const RboHudView *v, RboHudTextBegin begin,
			  RboHudTextFn text, RboHudTextEnd end)
{
	char line[48];
	u32 t = v->play_timer;
	u32 cs = t % 60;
	u32 sec = (t / 60) % 60;
	u32 min = t / 3600;
	int i;

	begin();

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 160});
	hud_solid(160.0f, 1.0f, 230.0f, 14.0f);
	hud_solid(392.0f, 1.0f, 128.0f, 14.0f);
	hud_solid(520.0f, 1.0f, 112.0f, 14.0f);

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
	if (v->stage_clear)
		snprintf(line, sizeof(line), "STAGE 1  CLEAR");
	else
		snprintf(line, sizeof(line), "STAGE 1 - AREA %d",
			 v->area > 0 ? v->area : 1);
	text(HUD_TOP_STAGE_X, HUD_TOP_STAGE_Y, line, 1.4f);

	snprintf(line, sizeof(line), "TIME %02u'%02u\"%02u",
		 min, sec, (cs * 100) / 60);
	text(HUD_TOP_TIME_X, HUD_TOP_TIME_Y, line, 1.4f);

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 220, 80, 255});
	text(HUD_TOP_ITEM_X, HUD_TOP_ITEM_Y, "x00 x00", 1.3f);

	for (i = 0; i < HUD_PARTY_SLOTS; i++) {
		f32 sx = k_sys_slot_x[i];
		const RboHudSlot *s = &v->slot[i];

		if (!s->occupied)
			continue;
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){20, 40, 120, 255});
		text(sx + 8.0f, HUD_SLOT_Y + 2.0f,
		     s->name ? s->name : "DIE", 1.3f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){20, 20, 20, 255});
		snprintf(line, sizeof(line), "%s LV%02d",
			 s->class_name ? s->class_name : "NOVICE",
			 s->level > 0 ? s->level : 1);
		text(sx + 8.0f, HUD_SLOT_Y + 12.0f, line, 1.1f);
		snprintf(line, sizeof(line), "%d/%d", s->hp, s->hp_max);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
		text(sx + 104.0f, HUD_SLOT_Y + 22.0f, line, 1.0f);
		snprintf(line, sizeof(line), "%d/%d", s->sp, s->sp_max);
		text(sx + 104.0f, HUD_SLOT_Y + 30.0f, line, 1.0f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){40, 80, 180, 255});
		text(sx + 148.0f, HUD_SLOT_Y + 2.0f, "DAM 0", 1.0f);
	}

	{
		snprintf(line, sizeof(line), "MEM %u%% %uK",
			 v->mem_pct, v->mem_free_k);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 200});
		hud_solid(500.0f, 430.0f, 132.0f, 20.0f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 240, 80, 255});
		text(HUD_MEM_X, HUD_MEM_Y, line, 1.4f);
		if (v->note && v->note[0]) {
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 200});
			hud_solid(6.0f, 404.0f, 360.0f, 32.0f);
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){180, 220, 255, 255});
			text(10.0f, 406.0f, v->note, 1.3f);
			if (v->note2 && v->note2[0])
				text(10.0f, 420.0f, v->note2, 1.3f);
		}
	}

	end();
}

void rbo_hud_draw(const RboHudView *v, GXTexObj *system,
		  RboHudTextBegin text_begin, RboHudTextFn text_fn,
		  RboHudTextEnd text_end)
{
	Mtx identity;

	if (!v || !system)
		return;

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	draw_follow_bars(v, system);
	draw_party_slots(v, system);
	draw_radar(v, system);
	if (text_begin && text_fn && text_end)
		draw_top_text(v, text_begin, text_fn, text_end);
}
