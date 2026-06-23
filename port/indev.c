// port/indev.c — LVGL keypad input device fed from the UART key queue.
#include "../kefyros.h"

static lv_indev_t *kbd;
static int grabbed = 0;
void kf_grab_input(int on){ grabbed = on; }

/* device keycode -> LVGL key. Emits REAL arrow keys; read_cb decides whether an
   UP/DOWN means "move list focus" (plain group) or "2-D move" (button matrix). */
uint32_t map_key(uint8_t k){
	switch(k){
	case DK_UP:        return LV_KEY_UP;
	case DK_DOWN:      return LV_KEY_DOWN;
	case DK_LEFT:      return LV_KEY_LEFT;
	case DK_RIGHT:     return LV_KEY_RIGHT;
	case DK_ENTER:     return LV_KEY_ENTER;
	case DK_ESC:       return LV_KEY_ESC;
	case DK_TAB:       return LV_KEY_NEXT;
	case DK_BACKSPACE: return LV_KEY_BACKSPACE;
	case DK_DEL:       return LV_KEY_DEL;
	case DK_HOME:      return LV_KEY_HOME;
	case DK_END:       return LV_KEY_END;
	default:           return (k>=0x20 && k<0x7f) ? k : 0;
	}
}

/* Should UP/DOWN move group focus (1-D), rather than reach the focused widget?
   Only true for list rows. The calc button-matrix, the home-screen grid tiles
   (2-D nav handled per-tile in launcher.c), and text areas (cursor) all want the
   RAW arrow keys instead. */
static int focused_is_list_item(lv_indev_t *indev){
	lv_group_t *g = lv_indev_get_group(indev);
	if(!g) return 0;
	lv_obj_t *f = lv_group_get_focused(g);
	if(!f) return 0;
	lv_obj_t *p = lv_obj_get_parent(f);
	return p && lv_obj_check_type(p, &lv_list_class);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data){
	static uint32_t last = 0;
	static int enter_down = 0;   /* hold ENTER PRESSED across polls -> native long-press */
	if(grabbed){ data->key=last; data->state=LV_INDEV_STATE_RELEASED; return; }
	uint8_t st, key;
	if(uart_pop_key(&st,&key)){
		uint32_t lk = map_key(key);
		/* POWER key -> power menu (shutdown/reboot), from any non-grabbing screen. */
		if(key==DK_POWER && st!=KS_RELEASE){ enter_down=0; kf_power_menu(); data->key=0; data->state=LV_INDEV_STATE_RELEASED; return; }
		/* ESC is the universal "back to launcher" for menu apps (terminal grabs
		   input, so this path never runs while it's active). */
		if(lk==LV_KEY_ESC && st!=KS_RELEASE){ enter_down=0; kf_back_to_launcher(); data->key=0; data->state=LV_INDEV_STATE_RELEASED; return; }
		/* On a plain list, UP/DOWN move group focus; swallow so LVGL doesn't also
		   route them to the focused widget. Matrices/grids/textareas keep raw arrows. */
		if((lk==LV_KEY_UP||lk==LV_KEY_DOWN) && st!=KS_RELEASE && focused_is_list_item(indev)){
			lv_group_t *g = lv_indev_get_group(indev);
			if(g){ if(lk==LV_KEY_UP) lv_group_focus_prev(g); else lv_group_focus_next(g); }
			data->key=0; data->state=LV_INDEV_STATE_RELEASED; return;
		}
		/* ENTER must stay PRESSED until the device sends a release, so LVGL's
		   long-press timer (400ms) can fire LV_EVENT_LONG_PRESSED (desktop move). */
		if(lk==LV_KEY_ENTER){
			enter_down = (st!=KS_RELEASE);
			last=lk; data->key=lk;
			data->state = enter_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
			return;
		}
		if(lk){ last=lk; data->key=lk;
			data->state = (st==KS_RELEASE) ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED; }
		else { data->key=last; data->state=LV_INDEV_STATE_RELEASED; }
	} else if(enter_down){
		data->key=LV_KEY_ENTER; data->state=LV_INDEV_STATE_PRESSED;
	} else {
		data->key=last; data->state=LV_INDEV_STATE_RELEASED;
	}
}

void indev_init(void){
	kbd = lv_indev_create();
	lv_indev_set_type(kbd, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(kbd, read_cb);
}
lv_indev_t *indev_get(void){ return kbd; }
