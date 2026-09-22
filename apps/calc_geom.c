// apps/calc_geom.c — PicoGeo: dynamic geometry engine for Kefyros PicoCalc.
// Interactive 2D coordinate geometry: vertices, lines, circles, midpoints,
// rubber-band previews, magnetic snapping, and direct grab-and-drag manipulation.
// Rendered on a black screen via 40-row direct RGB565 strip blitting.
#include "../kefyros.h"
#include "calc.h"
#include "calc_geom.h"
#include "disp.h"                 /* disp_pause_core1 / disp_resume_core1 */
#include "lcdspi/lcdspi.h"        /* draw_buffer_spi (direct panel blit) */
#include "pico/time.h"            /* time_us_64 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern char font8x8_basic[128][8];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GW 320
#define GH KF_CONTENT_H          /* 296 px: fit below persistent top bar */
#define STRIP_H 40
#undef RGB
#define RGB(r,g,b) (uint16_t)((((r)&0xf8)<<8)|(((g)&0xfc)<<3)|((b)>>3))

#define C_BG      RGB(0x0e,0x0c,0x0a)
#define C_GRID    RGB(0x24,0x20,0x18)
#define C_AXIS    RGB(0x5a,0x50,0x3c)
#define C_LBL     RGB(0x8a,0x7e,0x6e)
#define C_PT      RGB(0xff,0xc9,0x4d)   /* amber bright for vertices */
#define C_PT_GRAB RGB(0x4a,0xf0,0x70)   /* green for grabbed/dragged */
#define C_SEG     RGB(0x4a,0xc8,0xe0)   /* cyan for line segments */
#define C_LINE    RGB(0x38,0x98,0xb0)   /* dim cyan for infinite lines */
#define C_CIRC    RGB(0xb6,0xf0,0x00)   /* lime for circles */
#define C_CUR     RGB(0xff,0xff,0xff)   /* white for cursor */
#define C_SNAP    RGB(0x00,0xf0,0xf0)   /* bright cyan for snap box */
#define C_RUBBER  RGB(0xe0,0x6c,0x4a)   /* coral dashed preview */

/* ===================================================================== */
/* Entity Data Model                                                     */
/* ===================================================================== */
#define MAX_ENTITIES 48
#define NAME_MAX 8

typedef enum {
	G_NONE = 0,
	G_PT_FREE,          /* Free point: (x, y) */
	G_PT_MIDPOINT,      /* Midpoint of p[0] and p[1] */
	G_SEGMENT,          /* Line segment from p[0] to p[1] */
	G_LINE,             /* Infinite line through p[0] and p[1] */
	G_PERP,             /* Perpendicular through p[0] to line p[1] */
	G_PARALLEL,         /* Parallel through p[0] to line p[1] */
	G_CIRCLE_CP,        /* Circle: center p[0], radius through p[1] */
	G_POLYGON,          /* Polygon with nverts vertices: p[0..nverts-1] */
	G_MEASURE_DIST,     /* Distance readout between p[0] and p[1] */
	G_MEASURE_ANGLE     /* Angle at vertex p[1] between p[0] and p[2] */
} geom_type_t;

typedef struct {
	uint8_t     id;
	geom_type_t type;
	char        name[NAME_MAX];
	uint16_t    color;
	uint8_t     visible;
	uint8_t     aux;                /* e.g. vertex count for polygon */
	uint8_t     p[4];               /* parent entity IDs */
	uint8_t     nparents;

	/* Computed World Geometry */
	double      x, y;               /* position / center */
	double      dx, dy;             /* direction vector for lines */
	double      r;                  /* radius for circles */
	double      val;                /* measured length, area, or angle (deg) */
} geom_entity_t;

typedef enum {
	TOOL_NONE = 0,
	TOOL_POINT,
	TOOL_SEGMENT,
	TOOL_LINE,
	TOOL_CIRCLE,
	TOOL_MIDPOINT,
	TOOL_PERP,
	TOOL_PARALLEL,
	TOOL_POLY,
	TOOL_DIST,
	TOOL_ANGLE
} geom_tool_t;

static geom_entity_t s_ent[MAX_ENTITIES];
static int           s_nent = 0;
static char          s_next_pt_name = 'A';

/* Viewport state */
static double s_xmin = -10.0, s_xmax = 10.0;
static double s_ymin = -9.25, s_ymax = 9.25;

/* Interactive Cursor State */
static double s_cur_wx = 0.0, s_cur_wy = 0.0;
static double s_cur_vx = 0.0, s_cur_vy = 0.0;
static int    s_grabbed_id = -1;
static int    s_hover_id   = -1;
static int    s_snap_pt    = -1;
static int    s_grid_snap  = 0;

/* Tool State Machine */
static geom_tool_t s_tool = TOOL_NONE;
static int         s_tool_step = 0;
static uint8_t     s_staged_pts[4];

/* Display / Strip buffer */
static lv_obj_t *s_scr  = NULL;
static uint16_t *s_strip = NULL;
static int       s_cur_y0, s_cur_h;
static uint64_t  s_last_us = 0;

/* Velocity key tracking */
enum { K_L = 1, K_R = 2, K_U = 4, K_D = 8 };
static uint8_t s_held = 0;

/* ===================================================================== */
/* Coordinate Transformations                                            */
/* ===================================================================== */
static inline int SX(double wx){
	return (int)lround((wx - s_xmin) / (s_xmax - s_xmin) * (GW - 1));
}
static inline int SY(double wy){
	return (int)lround((GH - 1) - (wy - s_ymin) / (s_ymax - s_ymin) * (GH - 1));
}
static inline double WX(int sx){
	return s_xmin + (double)sx / (GW - 1) * (s_xmax - s_xmin);
}
static inline double WY(int sy){
	return s_ymin + (double)(GH - 1 - sy) / (GH - 1) * (s_ymax - s_ymin);
}

/* ===================================================================== */
/* Topological Constraint Solver                                         */
/* ===================================================================== */
static void geom_solve(void){
	for(int i = 0; i < s_nent; i++){
		geom_entity_t *e = &s_ent[i];
		switch(e->type){
		case G_PT_FREE:
			/* Keeps manual coordinates */
			break;
		case G_PT_MIDPOINT: {
			if(e->p[0] < s_nent && e->p[1] < s_nent){
				e->x = (s_ent[e->p[0]].x + s_ent[e->p[1]].x) * 0.5;
				e->y = (s_ent[e->p[0]].y + s_ent[e->p[1]].y) * 0.5;
			}
			break;
		}
		case G_SEGMENT:
		case G_LINE: {
			if(e->p[0] < s_nent && e->p[1] < s_nent){
				e->x = s_ent[e->p[0]].x;
				e->y = s_ent[e->p[0]].y;
				e->dx = s_ent[e->p[1]].x - s_ent[e->p[0]].x;
				e->dy = s_ent[e->p[1]].y - s_ent[e->p[0]].y;
				e->val = hypot(e->dx, e->dy);
			}
			break;
		}
		case G_PERP: {
			if(e->p[0] < s_nent && e->p[1] < s_nent){
				e->x = s_ent[e->p[0]].x;
				e->y = s_ent[e->p[0]].y;
				/* Rotate parent direction 90 degrees: (dx, dy) -> (-dy, dx) */
				e->dx = -s_ent[e->p[1]].dy;
				e->dy =  s_ent[e->p[1]].dx;
				e->val = hypot(e->dx, e->dy);
			}
			break;
		}
		case G_PARALLEL: {
			if(e->p[0] < s_nent && e->p[1] < s_nent){
				e->x = s_ent[e->p[0]].x;
				e->y = s_ent[e->p[0]].y;
				e->dx = s_ent[e->p[1]].dx;
				e->dy = s_ent[e->p[1]].dy;
				e->val = hypot(e->dx, e->dy);
			}
			break;
		}
		case G_CIRCLE_CP: {
			if(e->p[0] < s_nent && e->p[1] < s_nent){
				e->x = s_ent[e->p[0]].x;
				e->y = s_ent[e->p[0]].y;
				e->r = hypot(s_ent[e->p[1]].x - e->x, s_ent[e->p[1]].y - e->y);
				e->val = M_PI * e->r * e->r;
			}
			break;
		}
		case G_POLYGON: {
			/* Shoelace area formula */
			double area = 0;
			int nv = e->aux;
			for(int k = 0; k < nv; k++){
				int k2 = (k + 1) % nv;
				int id1 = e->p[k], id2 = e->p[k2];
				if(id1 < s_nent && id2 < s_nent)
					area += s_ent[id1].x * s_ent[id2].y - s_ent[id2].x * s_ent[id1].y;
			}
			e->val = fabs(area) * 0.5;
			break;
		}
		case G_MEASURE_DIST: {
			if(e->p[0] < s_nent && e->p[1] < s_nent)
				e->val = hypot(s_ent[e->p[1]].x - s_ent[e->p[0]].x, s_ent[e->p[1]].y - s_ent[e->p[0]].y);
			break;
		}
		case G_MEASURE_ANGLE: {
			if(e->p[0] < s_nent && e->p[1] < s_nent && e->p[2] < s_nent){
				double vx = s_ent[e->p[1]].x, vy = s_ent[e->p[1]].y;
				double a1 = atan2(s_ent[e->p[0]].y - vy, s_ent[e->p[0]].x - vx);
				double a2 = atan2(s_ent[e->p[2]].y - vy, s_ent[e->p[2]].x - vx);
				double deg = fabs(a2 - a1) * 180.0 / M_PI;
				if(deg > 180.0) deg = 360.0 - deg;
				e->val = deg;
			}
			break;
		}
		default: break;
		}
	}
}

/* ===================================================================== */
/* Entity Construction Helpers                                           */
/* ===================================================================== */
static int geom_alloc(geom_type_t t){
	if(s_nent >= MAX_ENTITIES) return -1;
	int id = s_nent++;
	memset(&s_ent[id], 0, sizeof(geom_entity_t));
	s_ent[id].id = id;
	s_ent[id].type = t;
	s_ent[id].visible = 1;
	return id;
}

static void move_point(int id, double dx, double dy){
	if(id < 0 || id >= s_nent) return;
	geom_entity_t *e = &s_ent[id];
	if(e->type == G_PT_FREE){
		e->x += dx;
		e->y += dy;
	} else if(e->type == G_PT_MIDPOINT){
		move_point(e->p[0], dx, dy);
		move_point(e->p[1], dx, dy);
	}
}

static int add_point_free(double wx, double wy){
	int id = geom_alloc(G_PT_FREE);
	if(id < 0) return -1;
	s_ent[id].x = wx;
	s_ent[id].y = wy;
	s_ent[id].color = C_PT;
	if(s_next_pt_name <= 'Z'){
		s_ent[id].name[0] = s_next_pt_name++;
		s_ent[id].name[1] = 0;
	} else {
		snprintf(s_ent[id].name, sizeof(s_ent[id].name), "P%d", id);
	}
	geom_solve();
	return id;
}

static int add_segment(uint8_t p1, uint8_t p2){
	if(p1 == p2) return -1;
	int id = geom_alloc(G_SEGMENT);
	if(id < 0) return -1;
	s_ent[id].p[0] = p1;
	s_ent[id].p[1] = p2;
	s_ent[id].nparents = 2;
	s_ent[id].color = C_SEG;
	snprintf(s_ent[id].name, sizeof(s_ent[id].name), "s%d", id);
	geom_solve();
	return id;
}

static int add_line(uint8_t p1, uint8_t p2){
	if(p1 == p2) return -1;
	int id = geom_alloc(G_LINE);
	if(id < 0) return -1;
	s_ent[id].p[0] = p1;
	s_ent[id].p[1] = p2;
	s_ent[id].nparents = 2;
	s_ent[id].color = C_LINE;
	snprintf(s_ent[id].name, sizeof(s_ent[id].name), "L%d", id);
	geom_solve();
	return id;
}

static int add_circle_cp(uint8_t center, uint8_t rim){
	if(center == rim) return -1;
	int id = geom_alloc(G_CIRCLE_CP);
	if(id < 0) return -1;
	s_ent[id].p[0] = center;
	s_ent[id].p[1] = rim;
	s_ent[id].nparents = 2;
	s_ent[id].color = C_CIRC;
	snprintf(s_ent[id].name, sizeof(s_ent[id].name), "c%d", id);
	geom_solve();
	return id;
}

static int add_midpoint(uint8_t p1, uint8_t p2){
	if(p1 == p2) return -1;
	int id = geom_alloc(G_PT_MIDPOINT);
	if(id < 0) return -1;
	s_ent[id].p[0] = p1;
	s_ent[id].p[1] = p2;
	s_ent[id].nparents = 2;
	s_ent[id].color = C_PT;
	if(s_next_pt_name <= 'Z'){
		s_ent[id].name[0] = s_next_pt_name++;
		s_ent[id].name[1] = 0;
	} else {
		snprintf(s_ent[id].name, sizeof(s_ent[id].name), "M%d", id);
	}
	geom_solve();
	return id;
}

static int add_perp(uint8_t pt, uint8_t line_id){
	int id = geom_alloc(G_PERP);
	if(id < 0) return -1;
	s_ent[id].p[0] = pt;
	s_ent[id].p[1] = line_id;
	s_ent[id].nparents = 2;
	s_ent[id].color = C_LINE;
	snprintf(s_ent[id].name, sizeof(s_ent[id].name), "p%d", id);
	geom_solve();
	return id;
}

static int add_polygon(const uint8_t *pts, int n){
	if(n < 3 || n > 4) return -1;
	int id = geom_alloc(G_POLYGON);
	if(id < 0) return -1;
	s_ent[id].aux = n;
	s_ent[id].nparents = n;
	for(int i = 0; i < n; i++) s_ent[id].p[i] = pts[i];
	s_ent[id].color = C_SEG;
	snprintf(s_ent[id].name, sizeof(s_ent[id].name), "poly%d", id);
	geom_solve();
	return id;
}

static void delete_entity(int target_id){
	if(target_id < 0 || target_id >= s_nent) return;
	/* Invalidate dependent entities */
	for(int i = 0; i < s_nent; i++){
		for(int k = 0; k < s_ent[i].nparents; k++){
			if(s_ent[i].p[k] == target_id){
				s_ent[i].type = G_NONE;
			}
		}
	}
	s_ent[target_id].type = G_NONE;
	if(s_grabbed_id == target_id) s_grabbed_id = -1;
	if(s_hover_id == target_id) s_hover_id = -1;
	geom_solve();
}

/* ===================================================================== */
/* Magnetic Snapping                                                     */
/* ===================================================================== */
static void update_snap(void){
	s_snap_pt = -1;
	s_hover_id = -1;
	int cur_sx = SX(s_cur_wx);
	int cur_sy = SY(s_cur_wy);

	/* 1. Snap to Points (Radius: 8 px) - skip vertex currently being dragged */
	int best_d2 = 8 * 8 + 1;
	for(int i = 0; i < s_nent; i++){
		if(s_ent[i].type == G_NONE) continue;
		if(i == s_grabbed_id) continue; /* NEVER snap to the vertex being dragged! */
		if(s_ent[i].type == G_PT_FREE || s_ent[i].type == G_PT_MIDPOINT){
			int sx = SX(s_ent[i].x);
			int sy = SY(s_ent[i].y);
			int dx = sx - cur_sx, dy = sy - cur_sy;
			int d2 = dx*dx + dy*dy;
			if(d2 < best_d2){
				best_d2 = d2;
				s_snap_pt = i;
				s_hover_id = i;
			}
		}
	}
	if(s_snap_pt >= 0 && s_grabbed_id < 0){
		s_cur_wx = s_ent[s_snap_pt].x;
		s_cur_wy = s_ent[s_snap_pt].y;
		return;
	}

	/* 2. Snap to Grid (if enabled) */
	if(s_grid_snap){
		double gstep = 0.5;
		s_cur_wx = round(s_cur_wx / gstep) * gstep;
		s_cur_wy = round(s_cur_wy / gstep) * gstep;
	}
}

/* ===================================================================== */
/* Drawing Primitives (Clipped to Current Strip)                         */
/* ===================================================================== */
static inline void px(int x, int y, uint16_t c){
	int yy = y - s_cur_y0;
	if((unsigned)x < GW && (unsigned)yy < (unsigned)s_cur_h)
		s_strip[yy * GW + x] = c;
}

static void line(int x0, int y0, int x1, int y1, uint16_t c){
	int yl = y0 < y1 ? y0 : y1, yh = y0 < y1 ? y1 : y0;
	if(yh < s_cur_y0 || yl >= s_cur_y0 + s_cur_h) return;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int e = dx + dy;
	for(;;){
		px(x0, y0, c);
		if(x0 == x1 && y0 == y1) break;
		int e2 = 2 * e;
		if(e2 >= dy){ e += dy; x0 += sx; }
		if(e2 <= dx){ e += dx; y0 += sy; }
	}
}

static void dashed_line(int x0, int y0, int x1, int y1, uint16_t c){
	int yl = y0 < y1 ? y0 : y1, yh = y0 < y1 ? y1 : y0;
	if(yh < s_cur_y0 || yl >= s_cur_y0 + s_cur_h) return;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int e = dx + dy, step = 0;
	for(;;){
		if((step++ & 7) < 4) px(x0, y0, c);
		if(x0 == x1 && y0 == y1) break;
		int e2 = 2 * e;
		if(e2 >= dy){ e += dy; x0 += sx; }
		if(e2 <= dx){ e += dx; y0 += sy; }
	}
}

static void circle(int xc, int yc, int r, uint16_t c){
	if(yc + r < s_cur_y0 || yc - r >= s_cur_y0 + s_cur_h) return;
	int x = 0, y = r, d = 3 - 2 * r;
	while(y >= x){
		px(xc + x, yc + y, c); px(xc - x, yc + y, c);
		px(xc + x, yc - y, c); px(xc - x, yc - y, c);
		px(xc + y, yc + x, c); px(xc - y, yc + x, c);
		px(xc + y, yc - x, c); px(xc - y, yc - x, c);
		x++;
		if(d > 0){ y--; d += 4 * (x - y) + 10; }
		else { d += 4 * x + 6; }
	}
}

static void dashed_circle(int xc, int yc, int r, uint16_t c){
	if(yc + r < s_cur_y0 || yc - r >= s_cur_y0 + s_cur_h) return;
	int x = 0, y = r, d = 3 - 2 * r, step = 0;
	while(y >= x){
		if((step++ & 3) < 2){
			px(xc + x, yc + y, c); px(xc - x, yc + y, c);
			px(xc + x, yc - y, c); px(xc - x, yc - y, c);
			px(xc + y, yc + x, c); px(xc - y, yc + x, c);
			px(xc + y, yc - x, c); px(xc - y, yc - x, c);
		}
		x++;
		if(d > 0){ y--; d += 4 * (x - y) + 10; }
		else { d += 4 * x + 6; }
	}
}

static void blit_ch(int x, int y, char ch, uint16_t c){
	if((unsigned char)ch >= 128) return;
	const char *g = font8x8_basic[(int)ch];
	for(int j = 0; j < 8; j++){
		uint8_t b = (uint8_t)g[j];
		for(int i = 0; i < 8; i++)
			if((b >> i) & 1) px(x + i, y + j, c);
	}
}

static void blit_str(int x, int y, const char *s, uint16_t c){
	for(; *s; s++, x += 6) blit_ch(x, y, *s, c);
}

static void draw_point_marker(int sx, int sy, uint16_t c, int is_grabbed){
	int r = is_grabbed ? 3 : 2;
	for(int dy = -r; dy <= r; dy++){
		for(int dx = -r; dx <= r; dx++){
			if(abs(dx) + abs(dy) <= r + 1)
				px(sx + dx, sy + dy, c);
		}
	}
}

/* ===================================================================== */
/* Scene Rasterizer                                                      */
/* ===================================================================== */
static void draw_scene(void){
	/* 1. Grid lines and axes */
	int ax0 = SX(0), ay0 = SY(0);
	double step = (s_xmax - s_xmin > 15.0) ? 2.0 : 1.0;

	for(double gx = ceil(s_xmin/step)*step; gx <= s_xmax; gx += step){
		int sx = SX(gx);
		for(int y = 0; y < GH; y += 4) px(sx, y, C_GRID);
	}
	for(double gy = ceil(s_ymin/step)*step; gy <= s_ymax; gy += step){
		int sy = SY(gy);
		for(int x = 0; x < GW; x += 4) px(x, sy, C_GRID);
	}
	if(ay0 >= 0 && ay0 < GH) for(int x = 0; x < GW; x++) px(x, ay0, C_AXIS);
	if(ax0 >= 0 && ax0 < GW) for(int y = 0; y < GH; y++) px(ax0, y, C_AXIS);

	/* 2. Geometric Entities */
	for(int i = 0; i < s_nent; i++){
		geom_entity_t *e = &s_ent[i];
		if(!e->visible || e->type == G_NONE) continue;

		switch(e->type){
		case G_SEGMENT: {
			int x0 = SX(s_ent[e->p[0]].x), y0 = SY(s_ent[e->p[0]].y);
			int x1 = SX(s_ent[e->p[1]].x), y1 = SY(s_ent[e->p[1]].y);
			line(x0, y0, x1, y1, e->color);
			break;
		}
		case G_LINE:
		case G_PERP:
		case G_PARALLEL: {
			/* Infinite line: clip through viewport edges */
			double len = hypot(e->dx, e->dy);
			if(len > 1e-9){
				double ux = e->dx / len * 40.0, uy = e->dy / len * 40.0;
				int x0 = SX(e->x - ux), y0 = SY(e->y - uy);
				int x1 = SX(e->x + ux), y1 = SY(e->y + uy);
				line(x0, y0, x1, y1, e->color);
			}
			break;
		}
		case G_CIRCLE_CP: {
			int cx = SX(e->x), cy = SY(e->y);
			int pr = (int)lround(e->r / (s_xmax - s_xmin) * (GW - 1));
			circle(cx, cy, pr, e->color);
			break;
		}
		case G_POLYGON: {
			int nv = e->aux;
			for(int k = 0; k < nv; k++){
				int k2 = (k + 1) % nv;
				int x0 = SX(s_ent[e->p[k]].x), y0 = SY(s_ent[e->p[k]].y);
				int x1 = SX(s_ent[e->p[k2]].x), y1 = SY(s_ent[e->p[k2]].y);
				line(x0, y0, x1, y1, e->color);
			}
			break;
		}
		default: break;
		}
	}

	/* 3. Rubber-Band Live Preview */
	if(s_tool_step > 0){
		int x0 = SX(s_ent[s_staged_pts[0]].x);
		int y0 = SY(s_ent[s_staged_pts[0]].y);
		int x1 = SX(s_cur_wx);
		int y1 = SY(s_cur_wy);
		if(s_tool == TOOL_SEGMENT || s_tool == TOOL_LINE){
			dashed_line(x0, y0, x1, y1, C_RUBBER);
		} else if(s_tool == TOOL_CIRCLE){
			int r = (int)lround(hypot(x1 - x0, y1 - y0));
			dashed_circle(x0, y0, r, C_RUBBER);
		} else if(s_tool == TOOL_POLY){
			for(int k = 0; k < s_tool_step - 1; k++){
				int p0 = SX(s_ent[s_staged_pts[k]].x),   q0 = SY(s_ent[s_staged_pts[k]].y);
				int p1 = SX(s_ent[s_staged_pts[k+1]].x), q1 = SY(s_ent[s_staged_pts[k+1]].y);
				line(p0, q0, p1, q1, C_SEG);
			}
			int last_x = SX(s_ent[s_staged_pts[s_tool_step-1]].x);
			int last_y = SY(s_ent[s_staged_pts[s_tool_step-1]].y);
			dashed_line(last_x, last_y, x1, y1, C_RUBBER);
		}
	}

	/* 4. Vertices & Labels */
	for(int i = 0; i < s_nent; i++){
		geom_entity_t *e = &s_ent[i];
		if(!e->visible || e->type == G_NONE) continue;
		if(e->type == G_PT_FREE || e->type == G_PT_MIDPOINT){
			int sx = SX(e->x), sy = SY(e->y);
			int is_grab = (i == s_grabbed_id);
			draw_point_marker(sx, sy, is_grab ? C_PT_GRAB : e->color, is_grab);
			blit_str(sx + 4, sy - 8, e->name, e->color);
		}
	}

	/* 5. Snap Bounding Box */
	if(s_snap_pt >= 0){
		int sx = SX(s_ent[s_snap_pt].x), sy = SY(s_ent[s_snap_pt].y);
		int b = 6;
		line(sx - b, sy - b, sx + b, sy - b, C_SNAP);
		line(sx + b, sy - b, sx + b, sy + b, C_SNAP);
		line(sx + b, sy + b, sx - b, sy + b, C_SNAP);
		line(sx - b, sy + b, sx - b, sy - b, C_SNAP);
	}

	/* 6. On-Screen Crosshair Cursor */
	int cx = SX(s_cur_wx), cy = SY(s_cur_wy);
	int cl = 5;
	line(cx - cl, cy, cx + cl, cy, C_CUR);
	line(cx, cy - cl, cx, cy + cl, C_CUR);

	/* 7. Live HUD & Readout Bar at Top */
	char hud[64];
	if(s_grabbed_id >= 0 && s_grabbed_id < s_nent){
		snprintf(hud, sizeof(hud), "[DRAGGING %s] (%.2f, %.2f)",
		         s_ent[s_grabbed_id].name, s_ent[s_grabbed_id].x, s_ent[s_grabbed_id].y);
	} else if(s_snap_pt >= 0){
		snprintf(hud, sizeof(hud), "[%s] (%.2f, %.2f) [SPACE:Grab]%s",
		         s_ent[s_snap_pt].name, s_ent[s_snap_pt].x, s_ent[s_snap_pt].y,
		         (s_ent[s_snap_pt].type == G_PT_MIDPOINT) ? " (mid)" : "");
	} else {
		snprintf(hud, sizeof(hud), "(%.2f, %.2f)%s", s_cur_wx, s_cur_wy, s_grid_snap ? " [GRID]" : "");
	}
	blit_str(4, 3, hud, (s_grabbed_id >= 0) ? C_PT_GRAB : C_PT);

	/* Active Tool Name */
	const char *tname = "";
	switch(s_tool){
	case TOOL_POINT:   tname = "TOOL: Point (click)"; break;
	case TOOL_SEGMENT: tname = (s_tool_step==0)?"TOOL: Seg (pt 1)":"TOOL: Seg (pt 2)"; break;
	case TOOL_LINE:    tname = (s_tool_step==0)?"TOOL: Line (pt 1)":"TOOL: Line (pt 2)"; break;
	case TOOL_CIRCLE:  tname = (s_tool_step==0)?"TOOL: Circle (ctr)":"TOOL: Circle (rim)"; break;
	case TOOL_MIDPOINT:tname = (s_tool_step==0)?"TOOL: Mid (pt 1)":"TOOL: Mid (pt 2)"; break;
	case TOOL_PERP:    tname = (s_tool_step==0)?"TOOL: Perp (pt)":"TOOL: Perp (line)"; break;
	case TOOL_POLY:    tname = "TOOL: Poly (pick pts)"; break;
	case TOOL_DIST:    tname = "TOOL: Dist (2 pts)"; break;
	case TOOL_ANGLE:   tname = "TOOL: Angle (3 pts)"; break;
	default:           tname = (s_grabbed_id>=0) ? "[DRAGGING]" : "SPACE:Grab P:Pt S:Seg C:Circ M:Mid"; break;
	}
	blit_str(4, GH - 10, tname, C_LBL);
}

/* Redraw scene strip-by-strip directly to panel with Core 1 paused */
static void redraw(void){
	if(!s_strip) return;
	disp_pause_core1();
	for(s_cur_y0 = 0; s_cur_y0 < GH; s_cur_y0 += STRIP_H){
		s_cur_h = (GH - s_cur_y0 < STRIP_H) ? (GH - s_cur_y0) : STRIP_H;
		for(int i = 0; i < GW * s_cur_h; i++) s_strip[i] = C_BG;
		draw_scene();
		draw_buffer_spi(0, KF_CONTENT_Y + s_cur_y0, GW - 1,
		                KF_CONTENT_Y + s_cur_y0 + s_cur_h - 1,
		                (unsigned char*)s_strip);
	}
	disp_resume_core1();
}

/* ===================================================================== */
/* Demonstration Starter Scene & Reset                                   */
/* ===================================================================== */
static void geom_reset(void){
	s_nent = 0;
	s_next_pt_name = 'A';
	s_xmin = -10.0; s_xmax = 10.0;
	s_ymin = -9.25; s_ymax = 9.25;
	s_cur_wx = 0.0; s_cur_wy = 0.0;
	s_cur_vx = 0.0; s_cur_vy = 0.0;
	s_held = 0;
	s_grabbed_id = -1;
	s_hover_id = -1;
	s_snap_pt = -1;
	s_grid_snap = 0;
	s_tool = TOOL_NONE;
	s_tool_step = 0;
	memset(s_staged_pts, 0, sizeof(s_staged_pts));
	memset(s_ent, 0, sizeof(s_ent));
}

static void populate_demo(void){
	geom_reset();
	int a = add_point_free(-4.0, -3.0);
	int b = add_point_free( 4.0, -3.0);
	int c = add_point_free( 0.0,  4.0);
	add_segment(a, b);
	add_segment(b, c);
	add_segment(c, a);
	add_midpoint(a, b);
	geom_solve();
}

/* ===================================================================== */
/* Public Engine API                                                     */
/* ===================================================================== */
void calc_geom_open(void){
	if(!s_strip) s_strip = malloc((size_t)GW * STRIP_H * 2);
	if(!s_strip){ calc_note("out of memory"); return; }

	populate_demo();

	s_scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(s_scr, 0, 0);
	lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
	lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

	calc_set_mode(CMODE_GEOM);
	lv_screen_load(s_scr);
	lv_refr_now(lv_display_get_default());
	s_last_us = time_us_64();
	redraw();
}

static void zoom(double f){
	double cx = (s_xmin + s_xmax) * 0.5, cy = (s_ymin + s_ymax) * 0.5;
	double hx = (s_xmax - s_xmin) * 0.5 * f, hy = (s_ymax - s_ymin) * 0.5 * f;
	s_xmin = cx - hx; s_xmax = cx + hx;
	s_ymin = cy - hy; s_ymax = cy + hy;
}

static double vel_step(double v, int dir, double acc, double vmax, double fric, double dt){
	if(dir){
		v += dir * acc * dt;
		if(v > vmax)  v = vmax;
		if(v < -vmax) v = -vmax;
	} else {
		double d = fric * dt;
		if(v > 0){ v -= d; if(v < 0) v = 0; }
		else if(v < 0){ v += d; if(v > 0) v = 0; }
	}
	return v;
}

/* Commit picked point to current active tool */
static void tool_commit_point(int pt_id){
	if(pt_id < 0) return;
	s_staged_pts[s_tool_step++] = (uint8_t)pt_id;

	switch(s_tool){
	case TOOL_SEGMENT:
		if(s_tool_step == 2){
			add_segment(s_staged_pts[0], s_staged_pts[1]);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	case TOOL_LINE:
		if(s_tool_step == 2){
			add_line(s_staged_pts[0], s_staged_pts[1]);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	case TOOL_CIRCLE:
		if(s_tool_step == 2){
			add_circle_cp(s_staged_pts[0], s_staged_pts[1]);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	case TOOL_MIDPOINT:
		if(s_tool_step == 2){
			add_midpoint(s_staged_pts[0], s_staged_pts[1]);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	case TOOL_PERP:
		if(s_tool_step == 2){
			/* Second target is line */
			add_perp(s_staged_pts[0], s_staged_pts[1]);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	case TOOL_POLY:
		if(s_tool_step >= 3 && pt_id == s_staged_pts[0]){
			/* Closed polygon */
			add_polygon(s_staged_pts, s_tool_step - 1);
			s_tool = TOOL_NONE; s_tool_step = 0;
		} else if(s_tool_step >= 4){
			add_polygon(s_staged_pts, 4);
			s_tool = TOOL_NONE; s_tool_step = 0;
		}
		break;
	default:
		s_tool = TOOL_NONE; s_tool_step = 0;
		break;
	}
}

void calc_geom_key(uint8_t key, int mods, int pressed){
	(void)mods;
	if(pressed){
		switch(key){
		case DK_ESC: case DK_BREAK:
			if(s_tool != TOOL_NONE || s_grabbed_id >= 0){
				s_tool = TOOL_NONE; s_tool_step = 0;
				s_grabbed_id = -1;
				redraw(); return;
			}
			geom_reset();
			free(s_strip); s_strip = NULL;
			lv_obj_t *old_scr = s_scr;
			s_scr = NULL;
			calc_show_worksheet();
			if(old_scr) lv_obj_delete(old_scr);
			return;

		case ' ':
		case DK_ENTER: {
			if(s_grabbed_id >= 0){
				/* Drop point */
				s_grabbed_id = -1;
				redraw(); return;
			}
			if(s_snap_pt >= 0){
				if(s_tool != TOOL_NONE){
					tool_commit_point(s_snap_pt);
				} else {
					/* Grab point */
					s_grabbed_id = s_snap_pt;
				}
			} else {
				/* Drop free point at cursor */
				int new_pt = add_point_free(s_cur_wx, s_cur_wy);
				if(s_tool != TOOL_NONE) tool_commit_point(new_pt);
			}
			redraw(); return;
		}

		/* Tool Selection Hotkeys */
		case 'p': case 'P': s_tool = TOOL_POINT; s_tool_step = 0; redraw(); return;
		case 's': case 'S': s_tool = TOOL_SEGMENT; s_tool_step = 0; redraw(); return;
		case 'l': case 'L': s_tool = TOOL_LINE; s_tool_step = 0; redraw(); return;
		case 'c': case 'C': s_tool = TOOL_CIRCLE; s_tool_step = 0; redraw(); return;
		case 'm': case 'M': s_tool = TOOL_MIDPOINT; s_tool_step = 0; redraw(); return;
		case 'r': case 'R':
			if(mods & MOD_SHIFT){
				s_xmin = -10.0; s_xmax = 10.0; s_ymin = -9.25; s_ymax = 9.25;
				s_cur_wx = 0; s_cur_wy = 0;
			} else {
				s_tool = TOOL_PERP; s_tool_step = 0;
			}
			redraw(); return;
		case 't': case 'T': s_tool = TOOL_POLY; s_tool_step = 0; redraw(); return;
		case 'g': case 'G': s_grid_snap = !s_grid_snap; redraw(); return;

		case DK_DEL:
		case DK_BACKSPACE:
			if(s_snap_pt >= 0) delete_entity(s_snap_pt);
			redraw(); return;

		case '+': case '=': zoom(0.8); redraw(); return;
		case '-': case '_': zoom(1.25); redraw(); return;
		}

		/* Arrow keys: immediate discrete step for single clicks / taps */
		if(key == DK_LEFT || key == DK_RIGHT || key == DK_UP || key == DK_DOWN){
			double step_x = (s_xmax - s_xmin) / (double)(GW - 1) * 4.0;
			double step_y = (s_ymax - s_ymin) / (double)(GH - 1) * 4.0;
			if(s_grid_snap){
				step_x = 0.5;
				step_y = 0.5;
			}
			if(mods & MOD_SHIFT){
				step_x *= 4.0;
				step_y *= 4.0;
			}
			if(key == DK_LEFT)  s_cur_wx -= step_x;
			if(key == DK_RIGHT) s_cur_wx += step_x;
			if(key == DK_UP)    s_cur_wy += step_y;
			if(key == DK_DOWN)  s_cur_wy -= step_y;

			/* Auto-scroll viewport if cursor reaches border */
			double margin_x = (s_xmax - s_xmin) * 0.1;
			double margin_y = (s_ymax - s_ymin) * 0.1;
			if(s_cur_wx < s_xmin + margin_x){ double shift = (s_xmin + margin_x) - s_cur_wx; s_xmin -= shift; s_xmax -= shift; }
			if(s_cur_wx > s_xmax - margin_x){ double shift = s_cur_wx - (s_xmax - margin_x); s_xmin += shift; s_xmax += shift; }
			if(s_cur_wy < s_ymin + margin_y){ double shift = (s_ymin + margin_y) - s_cur_wy; s_ymin -= shift; s_ymax -= shift; }
			if(s_cur_wy > s_ymax - margin_y){ double shift = s_cur_wy - (s_ymax - margin_y); s_ymin += shift; s_ymax += shift; }

			update_snap();

			/* If dragging a vertex, move it in real time and recompute DAG */
			if(s_grabbed_id >= 0 && s_grabbed_id < s_nent){
				double m_dx = s_cur_wx - s_ent[s_grabbed_id].x;
				double m_dy = s_cur_wy - s_ent[s_grabbed_id].y;
				move_point(s_grabbed_id, m_dx, m_dy);
				geom_solve();
			}

			redraw();

			/* Initial velocity kick so holding continues smoothly */
			if(key == DK_LEFT)  s_cur_vx = -0.4;
			if(key == DK_RIGHT) s_cur_vx =  0.4;
			if(key == DK_UP)    s_cur_vy =  0.4;
			if(key == DK_DOWN)  s_cur_vy = -0.4;
		}
	}

	/* Velocity cursor navigation */
	uint8_t bit = 0;
	switch(key){
	case DK_LEFT:  bit = K_L; break;
	case DK_RIGHT: bit = K_R; break;
	case DK_UP:    bit = K_U; break;
	case DK_DOWN:  bit = K_D; break;
	default: return;
	}
	if(pressed) s_held |= bit;
	else        s_held &= (uint8_t)~bit;
}

int calc_geom_tick(void){
	if(!s_scr || !s_strip) return 0;
	uint64_t now = time_us_64();
	double dt = (double)(now - s_last_us) / 1e6;
	s_last_us = now;
	if(dt <= 0) return 0;
	if(dt > 0.05) dt = 0.05;

	int dx = ((s_held & K_R)?1:0) - ((s_held & K_L)?1:0);
	int dy = ((s_held & K_U)?1:0) - ((s_held & K_D)?1:0);

	s_cur_vx = vel_step(s_cur_vx, dx, 5.0, 2.2, 7.0, dt);
	s_cur_vy = vel_step(s_cur_vy, dy, 5.0, 2.2, 7.0, dt);

	if(s_cur_vx == 0 && s_cur_vy == 0 && s_held == 0) return 0;

	double wx_span = s_xmax - s_xmin;
	double wy_span = s_ymax - s_ymin;
	s_cur_wx += s_cur_vx * wx_span * dt;
	s_cur_wy += s_cur_vy * wy_span * dt;

	/* Auto-scroll viewport if cursor reaches border while gliding */
	double margin_x = wx_span * 0.1;
	double margin_y = wy_span * 0.1;
	if(s_cur_wx < s_xmin + margin_x){ double shift = (s_xmin + margin_x) - s_cur_wx; s_xmin -= shift; s_xmax -= shift; }
	if(s_cur_wx > s_xmax - margin_x){ double shift = s_cur_wx - (s_xmax - margin_x); s_xmin += shift; s_xmax += shift; }
	if(s_cur_wy < s_ymin + margin_y){ double shift = (s_ymin + margin_y) - s_cur_wy; s_ymin -= shift; s_ymax -= shift; }
	if(s_cur_wy > s_ymax - margin_y){ double shift = s_cur_wy - (s_ymax - margin_y); s_ymin += shift; s_ymax += shift; }

	update_snap();

	/* If dragging a vertex, move it in real time and recompute DAG */
	if(s_grabbed_id >= 0 && s_grabbed_id < s_nent){
		double m_dx = s_cur_wx - s_ent[s_grabbed_id].x;
		double m_dy = s_cur_wy - s_ent[s_grabbed_id].y;
		move_point(s_grabbed_id, m_dx, m_dy);
		geom_solve();
	}

	redraw();
	return 1;
}
