// Bluetooth audio manager. Pair/connect only after an explicit row activation.
#include "../kefyros.h"
#include "../port/bt_audio.h"
#include "../ui/theme.h"
#include <stdint.h>
#include <stdio.h>

static lv_obj_t *scr, *status_label, *list;
static lv_group_t *group;
static lv_timer_t *timer;
static int shown_scans=-1, shown_saved=-1;

static void update_status(void){
    if(!status_label) return;
    if(kf_bt_state()==KF_BT_CONNECTED)
        lv_label_set_text_fmt(status_label,"BT: %s  [SBC]",kf_bt_device_name());
    else lv_label_set_text_fmt(status_label,"BT: %s   output: speaker",kf_bt_state_text());
    lv_obj_set_style_text_color(status_label,
        kf_bt_state()==KF_BT_CONNECTED ? KF_ACTIVE : KF_AMBER,0);
}
static void act_scan(lv_event_t *e){ (void)e; kf_bt_scan(); update_status(); }
static void act_speaker(lv_event_t *e){ (void)e; kf_bt_disconnect(); update_status(); }
static void act_saved(lv_event_t *e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    kf_bt_connect_saved(i); update_status();
}
static void act_found(lv_event_t *e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    kf_bt_connect_scan(i); update_status();
}
static void act_forget(lv_event_t *e){
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    kf_bt_forget_saved(i); shown_saved=-1;
}
static lv_obj_t *row(const char *text,lv_event_cb_t cb,int index){
    lv_obj_t *b=lv_list_add_button(list,NULL,text);
    lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,(void*)(intptr_t)index);
    lv_group_add_obj(group,b);
    return b;
}
static void rebuild(void){
    if(!list) return;
    lv_obj_clean(list);
    row("Use speaker",act_speaker,0);
    row("Scan for audio devices",act_scan,0);
    int n=kf_bt_saved_count();
    for(int i=0;i<n;i++){
        char label[80];
        snprintf(label,sizeof label,"Connect: %s",kf_bt_saved_name(i));
        row(label,act_saved,i);
        snprintf(label,sizeof label,"Forget: %s",kf_bt_saved_name(i));
        row(label,act_forget,i);
    }
    int scans=kf_bt_scan_count();
    for(int i=0;i<scans;i++){
        char label[80];
        snprintf(label,sizeof label,"Found: %s",kf_bt_scan_name(i));
        row(label,act_found,i);
    }
    shown_saved=n; shown_scans=scans;
    lv_obj_t *first=lv_obj_get_child(list,0);
    if(first) lv_group_focus_obj(first);
}
static void tick(lv_timer_t *t){
    (void)t;
    if(!scr || lv_screen_active()!=scr) return;
    update_status();
    if(shown_saved!=kf_bt_saved_count() || shown_scans!=kf_bt_scan_count()) rebuild();
}
static void on_delete(lv_event_t *e){
    (void)e;
    if(timer){ lv_timer_delete(timer); timer=NULL; }
    scr=NULL; list=NULL; status_label=NULL;
}
void app_bluetooth_open(void){
    kf_bt_init();
    scr=lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr,0,0);
    kf_inset_top(scr);
    lv_obj_add_event_cb(scr,on_delete,LV_EVENT_DELETE,NULL);
    status_label=lv_label_create(scr);
    lv_obj_set_width(status_label,LCD_W-16);
    lv_label_set_long_mode(status_label,LV_LABEL_LONG_DOT);
    lv_obj_align(status_label,LV_ALIGN_TOP_LEFT,8,8);
    list=lv_list_create(scr);
    lv_obj_set_size(list,LCD_W-8,KF_CONTENT_H-36);
    lv_obj_align(list,LV_ALIGN_TOP_MID,0,32);
    group=kf_use_group();
    shown_saved=shown_scans=-1;
    rebuild(); update_status();
    timer=lv_timer_create(tick,400,NULL);
    lv_screen_load(scr);
}
