// ui/power.c — power menu screen (Shutdown / Reboot / Cancel). Reached via the POWER
// key (port/indev.c) or the Settings app. It's a normal screen, so ESC returns to the
// launcher; the list gives free d-pad navigation.
#include "../kefyros.h"
#include "theme.h"
#include <stdlib.h>

static lv_obj_t *scr;

static void act_off(lv_event_t *e){ (void)e; kf_poweroff(); }
static void act_reboot(lv_event_t *e){ (void)e; kf_reboot(); }
static void act_bootsel(lv_event_t *e){ (void)e; kf_bootsel(); }
static void act_cancel(lv_event_t *e){ (void)e; launcher_show(); }

void kf_power_menu(void){
	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);                  /* clear the persistent OS top bar */

	lv_obj_t *t = lv_label_create(scr);
	lv_label_set_text(t, "Power");
	lv_obj_set_style_text_color(t, KF_AMBER_BR, 0);
	lv_obj_set_style_text_font(t, KF_FONT_BIG, 0);
	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

	lv_obj_t *list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-44);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);

	lv_group_t *g = kf_use_group();
	lv_obj_t *b;
	b = lv_list_add_button(list, NULL, "Shutdown"); lv_obj_add_event_cb(b, act_off,    LV_EVENT_CLICKED, NULL); lv_group_add_obj(g, b);
	b = lv_list_add_button(list, NULL, "Reboot");   lv_obj_add_event_cb(b, act_reboot, LV_EVENT_CLICKED, NULL); lv_group_add_obj(g, b);
	b = lv_list_add_button(list, NULL, "BOOTSEL");  lv_obj_add_event_cb(b, act_bootsel,LV_EVENT_CLICKED, NULL); lv_group_add_obj(g, b);
	b = lv_list_add_button(list, NULL, "Cancel");   lv_obj_add_event_cb(b, act_cancel, LV_EVENT_CLICKED, NULL); lv_group_add_obj(g, b);
	lv_group_focus_obj(lv_obj_get_child(list, 0));

	lv_screen_load(scr);
}
