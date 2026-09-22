#ifndef KF_BT_AUDIO_H
#define KF_BT_AUDIO_H

/* Bluetooth Classic A2DP source. All calls run on Core 0. Connections are only
   initiated by a UI action; remembered devices are never auto-connected. */
typedef enum {
    KF_BT_OFF = 0, KF_BT_STARTING, KF_BT_READY, KF_BT_SCANNING,
    KF_BT_CONNECTING, KF_BT_CONNECTED, KF_BT_FAILED
} kf_bt_state_t;

int kf_bt_init(void);
void kf_bt_poll(void);
kf_bt_state_t kf_bt_state(void);
const char *kf_bt_state_text(void);
const char *kf_bt_device_name(void);
int kf_bt_scan(void);
int kf_bt_scan_count(void);
const char *kf_bt_scan_name(int index);
const char *kf_bt_scan_address(int index);
int kf_bt_connect_scan(int index);
int kf_bt_saved_count(void);
const char *kf_bt_saved_name(int index);
const char *kf_bt_saved_address(int index);
int kf_bt_connect_saved(int index);
void kf_bt_forget_saved(int index);
void kf_bt_disconnect(void);

#endif
