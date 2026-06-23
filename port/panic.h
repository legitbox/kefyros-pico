// port/panic.h — minimal declaration for LV_ASSERT_HANDLER_INCLUDE so lv_conf.h
// can route LVGL assertions to the amber screen of death without dragging the
// whole of kefyros.h / lvgl.h into the assert path.
#ifndef KF_PANIC_H
#define KF_PANIC_H
void kf_panic(const char *title, const char *l1, const char *l2);
#endif
