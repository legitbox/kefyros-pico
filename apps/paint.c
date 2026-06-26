// apps/paint.c — Kefyros Paint: a keyboard-driven pixel-art editor.
// PHASE 1 (skeleton + render spike): an LV_COLOR_FORMAT_I4 (16-colour indexed) canvas
// shown integer-zoom-scaled with antialiasing OFF (crisp nearest-neighbour pixels) so we
// never allocate a full-screen framebuffer. Cursor + pencil/eraser + grid/HUD toggles.
//
// The art buffer IS the document: an lv_draw_buf (I4) we own, displayed by an lv_canvas
// scaled to fit. Undo/PSRAM persistence/shapes/fill come in later phases (see PAINT_PLAN.md).
//
// Like calc/editor this app GRABS the keyboard (kf_grab_input) and reads raw device keys
// from paint_poll() (pumped in the superloop). Controls:
//   arrows move cursor · Space toggle pen-down (draw while moving) · Enter stamp one cell
//   p pencil · e eraser · [ ] cycle colour · + - zoom · g grid · h HUD · n cycle size (temp)
//   Esc quit
#include "../kefyros.h"
#include "../ui/theme.h"
#include <string.h>

/* ---- 16-colour default palette (PICO-8 set; every slot editable later) ---- */
static const uint32_t PAL[16] = {
	0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
	0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA,
};

/* preset square sizes (Phase 1 cycles these with 'n'; Phase 5 gives a proper New picker) */
static const int SIZES[] = { 16, 32, 64, 128, 144 };
#define NSIZES (int)(sizeof(SIZES)/sizeof(SIZES[0]))
#define GRID_MIN_ZOOM 6      /* only draw the pixel grid once cells are big enough */

enum { TOOL_PENCIL = 0, TOOL_ERASER };

static int            g_active = 0;
static lv_obj_t      *g_scr, *g_clip, *g_canvas, *g_hud;
static lv_draw_buf_t *g_db;                       /* the I4 document buffer (we own/free it) */
static int g_w = 64, g_h = 64, g_size_i = 2;      /* default 64x64 */
static int g_zoom = 4, g_cx = 32, g_cy = 32;
static int g_color = 7, g_tool = TOOL_PENCIL;     /* default white pencil */
static int g_grid = 1, g_hud_on = 1, g_pen = 0;   /* pen = draw-while-moving */
static int g_ox, g_oy;                            /* scaled-canvas top-left, relative to clip */

/* ---------------- I4 pixel access (direct nibble, matches lv_canvas_set_px layout) -------- */
static inline void px_set(int x, int y, int idx){
	if(x < 0 || y < 0 || x >= g_w || y >= g_h) return;
	uint8_t *p = (uint8_t*)g_db->data + (uint32_t)y * g_db->header.stride + (x >> 1);
	uint8_t shift = (x & 1) ? 0 : 4;              /* even x = high nibble (lv: 4 - 4*(x&1)) */
	*p = (uint8_t)((*p & ~(0xF << shift)) | ((idx & 0xF) << shift));
}

/* ---------------- layout: size + position the scaled canvas (follow the cursor) ----------- */
static void recompute_zoom(void){
	int zw = LCD_W / g_w, zh = KF_CONTENT_H / g_h;
	g_zoom = zw < zh ? zw : zh;
	if(g_zoom < 1)  g_zoom = 1;
	if(g_zoom > 32) g_zoom = 32;
}
static void relayout(void){
	int vw = g_w * g_zoom, vh = g_h * g_zoom;     /* on-screen size of the scaled canvas */
	if(vw <= LCD_W) g_ox = (LCD_W - vw) / 2;      /* fits: centre */
	else {                                        /* overflows: keep cursor in view */
		g_ox = LCD_W / 2 - (g_cx * g_zoom + g_zoom / 2);
		if(g_ox > 0) g_ox = 0; if(g_ox < LCD_W - vw) g_ox = LCD_W - vw;
	}
	if(vh <= KF_CONTENT_H) g_oy = (KF_CONTENT_H - vh) / 2;
	else {
		g_oy = KF_CONTENT_H / 2 - (g_cy * g_zoom + g_zoom / 2);
		if(g_oy > 0) g_oy = 0; if(g_oy < KF_CONTENT_H - vh) g_oy = KF_CONTENT_H - vh;
	}
	/* transform-scale the native-size canvas (pivot is top-left, set in rebuild). The object
	   stays g_w x g_h; the scaled image grows down-right from (g_ox,g_oy) to fill vw x vh. */
	lv_image_set_scale(g_canvas, (uint32_t)(g_zoom * 256));   /* 256 = 1x */
	lv_obj_set_pos(g_canvas, g_ox, g_oy);
	lv_obj_invalidate(g_clip);                    /* redraw image + overlay */
}

/* ---------------- HUD ---------------- */
static void update_hud(void){
	if(!g_hud) return;
	lv_label_set_text_fmt(g_hud, "%s  c%d  (%d,%d)  %dx  %dx%d%s",
		g_tool == TOOL_ERASER ? "ERASE" : "PENCIL",
		g_color, g_cx, g_cy, g_zoom, g_w, g_h, g_pen ? "  PEN" : "");
}

/* ---------------- overlay: grid + cursor drawn over the canvas (never touch the buffer) --- */
static void clip_draw_post(lv_event_t *e){
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t cl; lv_obj_get_coords(g_clip, &cl);     /* clip's absolute origin (pad 0) */
	int bx = cl.x1 + g_ox, by = cl.y1 + g_oy;         /* scaled-canvas top-left, absolute */
	int vw = g_w * g_zoom, vh = g_h * g_zoom;

	if(g_grid && g_zoom >= GRID_MIN_ZOOM){
		lv_draw_rect_dsc_t gd; lv_draw_rect_dsc_init(&gd);
		gd.bg_color = KF_BORDER; gd.bg_opa = LV_OPA_40; gd.border_width = 0;
		for(int i = 0; i <= g_w; i++){ int x = bx + i * g_zoom;
			lv_area_t v = { x, by, x, by + vh - 1 }; lv_draw_rect(layer, &gd, &v); }
		for(int j = 0; j <= g_h; j++){ int y = by + j * g_zoom;
			lv_area_t hh = { bx, y, bx + vw - 1, y }; lv_draw_rect(layer, &gd, &hh); }
	}

	lv_draw_rect_dsc_t cd; lv_draw_rect_dsc_init(&cd);
	cd.bg_opa = LV_OPA_TRANSP; cd.border_color = KF_ACTIVE;
	cd.border_width = g_zoom >= 4 ? 2 : 1; cd.border_opa = LV_OPA_COVER;
	lv_area_t a = { bx + g_cx * g_zoom, by + g_cy * g_zoom,
	                bx + g_cx * g_zoom + g_zoom - 1, by + g_cy * g_zoom + g_zoom - 1 };
	lv_draw_rect(layer, &cd, &a);
}

/* ---------------- (re)build the document buffer at w x h ---------------- */
static void rebuild_canvas(int w, int h){
	if(g_db){ lv_draw_buf_destroy(g_db); g_db = NULL; }
	g_w = w; g_h = h;
	g_db = lv_draw_buf_create(g_w, g_h, LV_COLOR_FORMAT_I4, 0);
	memset(g_db->data, 0, (size_t)g_db->header.stride * g_h);   /* all index 0 = background */
	lv_canvas_set_draw_buf(g_canvas, g_db);
	for(int i = 0; i < 16; i++)
		lv_canvas_set_palette(g_canvas, (uint8_t)i, lv_color_to_32(lv_color_hex(PAL[i]), 0xFF));
	lv_obj_set_size(g_canvas, g_w, g_h);          /* native size; transform-scale enlarges it */
	lv_image_set_pivot(g_canvas, 0, 0);           /* scale grows down-right from the top-left */
	lv_image_set_antialias(g_canvas, false);      /* crisp nearest-neighbour pixels */
	g_cx = g_w / 2; g_cy = g_h / 2;
	recompute_zoom();
	relayout();
}

static void stamp(void){
	px_set(g_cx, g_cy, g_tool == TOOL_ERASER ? 0 : g_color);
	lv_obj_invalidate(g_clip);    /* redraws the canvas image (child) + overlay */
}
static void move_cursor(int dx, int dy){
	g_cx += dx; g_cy += dy;
	if(g_cx < 0) g_cx = 0; if(g_cx >= g_w) g_cx = g_w - 1;
	if(g_cy < 0) g_cy = 0; if(g_cy >= g_h) g_cy = g_h - 1;
	if(g_pen) stamp();
	relayout();                       /* follow cursor if zoomed past the screen */
	lv_obj_invalidate(g_clip);        /* redraw the cursor/grid overlay */
	update_hud();
}

/* ---------------- key handling (raw device keys via paint_poll) ---------------- */
static void exit_to_launcher(void){
	g_active = 0; kf_grab_input(0);
	if(g_db){ lv_draw_buf_destroy(g_db); g_db = NULL; }  /* free our buffer; launcher frees g_scr */
	kf_back_to_launcher();
}
static void handle_key(uint8_t k, int mods){
	(void)mods;
	switch(k){
	case DK_ESC: case DK_BREAK: exit_to_launcher(); return;
	case DK_LEFT:  move_cursor(-1, 0); return;
	case DK_RIGHT: move_cursor( 1, 0); return;
	case DK_UP:    move_cursor( 0,-1); return;
	case DK_DOWN:  move_cursor( 0, 1); return;
	case DK_ENTER: stamp(); return;
	case ' ':      g_pen = !g_pen; if(g_pen) stamp(); update_hud(); return;
	case 'p': case 'P': g_tool = TOOL_PENCIL; update_hud(); return;
	case 'e': case 'E': g_tool = TOOL_ERASER; update_hud(); return;
	case '[': g_color = (g_color + 15) & 0xF; update_hud(); lv_obj_invalidate(g_clip); return;
	case ']': g_color = (g_color + 1)  & 0xF; update_hud(); lv_obj_invalidate(g_clip); return;
	case '+': case '=': if(g_zoom < 32){ g_zoom++; relayout(); lv_obj_invalidate(g_clip); update_hud(); } return;
	case '-': case '_': if(g_zoom > 1){  g_zoom--; relayout(); lv_obj_invalidate(g_clip); update_hud(); } return;
	case 'g': case 'G': g_grid = !g_grid; lv_obj_invalidate(g_clip); return;
	case 'h': case 'H': g_hud_on = !g_hud_on;
		if(g_hud_on){ lv_obj_clear_flag(g_hud, LV_OBJ_FLAG_HIDDEN); update_hud(); }
		else lv_obj_add_flag(g_hud, LV_OBJ_FLAG_HIDDEN); return;
	case 'n': case 'N':                       /* TEMP spike harness: cycle preset sizes */
		g_size_i = (g_size_i + 1) % NSIZES;
		rebuild_canvas(SIZES[g_size_i], SIZES[g_size_i]);
		lv_obj_invalidate(g_clip); update_hud(); return;
	}
}
void paint_poll(void){
	if(!g_active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		handle_key(key, uart_mods());
	}
}

/* ---------------- entry ---------------- */
void app_paint_open(void){
	g_active = 1; g_db = NULL;
	g_size_i = 2; g_tool = TOOL_PENCIL; g_color = 7; g_grid = 1; g_hud_on = 1; g_pen = 0;

	g_scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(g_scr, 0, 0);
	lv_obj_set_style_bg_color(g_scr, KF_BG_DEEP, 0);
	lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

	/* clip container = the content area below the OS top bar; crops the overflowing canvas */
	g_clip = lv_obj_create(g_scr);
	lv_obj_remove_style_all(g_clip);
	lv_obj_set_size(g_clip, LCD_W, KF_CONTENT_H);
	lv_obj_set_pos(g_clip, 0, KF_CONTENT_Y);
	lv_obj_set_style_bg_color(g_clip, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(g_clip, LV_OPA_COVER, 0);
	lv_obj_clear_flag(g_clip, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(g_clip, clip_draw_post, LV_EVENT_DRAW_POST, NULL);

	g_canvas = lv_canvas_create(g_clip);
	lv_obj_set_style_pad_all(g_canvas, 0, 0);
	lv_obj_set_style_border_width(g_canvas, 0, 0);

	/* HUD readout (toggle with 'h') */
	g_hud = lv_label_create(g_scr);
	lv_obj_set_style_text_font(g_hud, KF_FONT, 0);
	lv_obj_set_style_text_color(g_hud, KF_AMBER, 0);
	lv_obj_set_style_bg_color(g_hud, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(g_hud, LV_OPA_70, 0);
	lv_obj_set_style_pad_hor(g_hud, 3, 0);
	lv_obj_set_pos(g_hud, 2, KF_CONTENT_Y + 2);

	rebuild_canvas(SIZES[g_size_i], SIZES[g_size_i]);
	update_hud();

	lv_screen_load(g_scr);
	kf_grab_input(1);
}
