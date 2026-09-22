// Bluetooth Classic A2DP source for the CYW43439. Deliberately Core-0 only:
// BTstack shares the existing polled CYW43 context with lwIP.
#include "bt_audio.h"
#include "../kefyros.h"
#include "../ui/deskconf.h"
#include "btstack.h"
#include <stdio.h>
#include <string.h>

#define BT_SCAN_MAX 16
#define BT_SAVED_MAX 6
#define SBC_PACKET_CAP 1030

typedef struct { bd_addr_t addr; char name[40]; int8_t rssi; } bt_device_t;
static bt_device_t scanned[BT_SCAN_MAX], saved[BT_SAVED_MAX];
static int scan_count, saved_count, saved_loaded;
static bt_device_t target;
static kf_bt_state_t state = KF_BT_OFF;
static int initialized, hci_ready, pending_save, pending_scan, pending_connect, eco_hold;
static uint16_t a2dp_cid;
static uint8_t local_seid;
static int stream_started, codec_rate = 44100;
static const btstack_sbc_encoder_t *encoder;
static btstack_sbc_encoder_bluedroid_t encoder_ctx;
static btstack_packet_callback_registration_t hci_callback;
static uint8_t codec_config[4];
static uint8_t codec_capabilities[4] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) |
        AVDTP_SBC_JOINT_STEREO | AVDTP_SBC_STEREO,
    0xff, 2, 53
};
static uint8_t sdp_record[150];
static uint8_t packet[SBC_PACKET_CAP];
static int16_t pcm[256 * 2];
static uint16_t packet_bytes;
static uint8_t packet_frames, send_pending;
static uint32_t timestamp, due_frames, due_remainder, last_ms;
static btstack_timer_source_t audio_timer;

static void address_text(const bd_addr_t addr, char out[18]){
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}
static int parse_address(const char *s, bd_addr_t out){
    unsigned x[6];
    if(!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                    &x[0], &x[1], &x[2], &x[3], &x[4], &x[5]) != 6) return 0;
    for(int i=0;i<6;i++) out[i]=(uint8_t)x[i];
    return 1;
}
static void load_saved(void){
    if(saved_loaded) return;
    saved_loaded=1;
    for(int i=0;i<BT_SAVED_MAX;i++){
        char key[24];
        snprintf(key,sizeof key,"bt.%d.addr",i);
        const char *addr=deskconf_get(key,"");
        bd_addr_t parsed;
        if(!parse_address(addr,parsed)) continue;
        bt_device_t *d=&saved[saved_count++];
        memcpy(d->addr,parsed,sizeof d->addr);
        snprintf(key,sizeof key,"bt.%d.name",i);
        snprintf(d->name,sizeof d->name,"%s",deskconf_get(key,addr));
    }
}
static void persist_saved(void){
    for(int i=0;i<BT_SAVED_MAX;i++){
        char key[24], addr[18];
        snprintf(key,sizeof key,"bt.%d.addr",i);
        if(i<saved_count){ address_text(saved[i].addr,addr); deskconf_set(key,addr); }
        else deskconf_set(key,"");
        snprintf(key,sizeof key,"bt.%d.name",i);
        deskconf_set(key,i<saved_count?saved[i].name:"");
    }
}
static void remember_target(void){
    load_saved();
    int pos=-1;
    for(int i=0;i<saved_count;i++) if(!memcmp(saved[i].addr,target.addr,6)){ pos=i; break; }
    if(pos<0){
        if(saved_count<BT_SAVED_MAX) pos=saved_count++;
        else pos=BT_SAVED_MAX-1;
    }
    saved[pos]=target;
    persist_saved();
}
static void stop_stream(void){
    btstack_run_loop_remove_timer(&audio_timer);
    stream_started=0; packet_bytes=packet_frames=send_pending=0;
    due_frames=due_remainder=last_ms=0;
    kf_audio_set_bt_route(0);
}
static void send_packet(void){
    if(!stream_started || !packet_frames) return;
    packet[0]=packet_frames;
    uint8_t status=a2dp_source_stream_send_media_payload_rtp(
        a2dp_cid,local_seid,0,timestamp,packet,packet_bytes+1);
    if(status==ERROR_CODE_SUCCESS){
        timestamp+=(uint32_t)packet_frames*encoder->num_audio_frames(&encoder_ctx);
        packet_bytes=0; packet_frames=0;
    }
    send_pending=0;
}
static void timer_tick(btstack_timer_source_t *timer){
    (void)timer;
    if(!stream_started || !encoder) return;
    btstack_run_loop_set_timer(&audio_timer,10);
    btstack_run_loop_add_timer(&audio_timer);
    uint32_t now=btstack_run_loop_get_time_ms();
    uint32_t elapsed=last_ms ? now-last_ms : 10;
    last_ms=now;
    if(elapsed>40) elapsed=40;
    due_remainder+=elapsed*(uint32_t)codec_rate;
    due_frames+=due_remainder/1000;
    due_remainder%=1000;
    if(due_frames>2048) due_frames=2048;
    if(send_pending) return;
    uint16_t sbc_bytes=encoder->sbc_buffer_length(&encoder_ctx);
    uint16_t sbc_frames=encoder->num_audio_frames(&encoder_ctx);
    int payload_limit=a2dp_max_media_payload_size(a2dp_cid,local_seid);
    if(payload_limit>SBC_PACKET_CAP) payload_limit=SBC_PACKET_CAP;
    while(sbc_frames && due_frames>=sbc_frames &&
          packet_bytes+sbc_bytes+1 <= payload_limit){
        memset(pcm,0,sbc_frames*2*sizeof(int16_t));
        kf_audio_bt_read(pcm,sbc_frames,codec_rate);
        if(encoder->encode_signed_16(&encoder_ctx,pcm,&packet[packet_bytes+1])!=ERROR_CODE_SUCCESS){
            kf_bt_disconnect(); state=KF_BT_FAILED; return;
        }
        packet_bytes+=sbc_bytes;
        packet_frames++;
        due_frames-=sbc_frames;
    }
    if(packet_frames){
        send_pending=1;
        a2dp_source_stream_endpoint_request_can_send_now(a2dp_cid,local_seid);
    }
}
static void a2dp_event(uint8_t type,uint16_t channel,uint8_t *event,uint16_t size){
    (void)channel; (void)size;
    if(type!=HCI_EVENT_PACKET || hci_event_packet_get_type(event)!=HCI_EVENT_A2DP_META) return;
    uint8_t sub=hci_event_a2dp_meta_get_subevent_code(event);
    switch(sub){
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        if(a2dp_subevent_signaling_connection_established_get_status(event)!=ERROR_CODE_SUCCESS){
            a2dp_cid=0; state=KF_BT_FAILED;
        } else a2dp_cid=a2dp_subevent_signaling_connection_established_get_a2dp_cid(event);
        break;
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
        codec_rate=a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(event);
        int blocks=a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(event);
        int bands=a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(event);
        int bitpool=a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(event);
        int allocation=a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(event)-1;
        int mode=a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(event);
        btstack_sbc_channel_mode_t sbc_mode=mode==AVDTP_CHANNEL_MODE_JOINT_STEREO ?
            SBC_CHANNEL_MODE_JOINT_STEREO : SBC_CHANNEL_MODE_STEREO;
        encoder=btstack_sbc_encoder_bluedroid_init_instance(&encoder_ctx);
        if(encoder->configure(&encoder_ctx,SBC_MODE_STANDARD,blocks,bands,
                              (btstack_sbc_allocation_method_t)allocation,
                              codec_rate,bitpool,sbc_mode)!=ERROR_CODE_SUCCESS){
            encoder=NULL; state=KF_BT_FAILED;
        }
        break;
    }
    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        if(a2dp_subevent_stream_established_get_status(event)!=ERROR_CODE_SUCCESS){
            state=KF_BT_FAILED; break;
        }
        a2dp_cid=a2dp_subevent_stream_established_get_a2dp_cid(event);
        a2dp_source_start_stream(a2dp_cid,local_seid);
        break;
    case A2DP_SUBEVENT_STREAM_STARTED:
        if(!encoder){ state=KF_BT_FAILED; break; }
        stream_started=1; state=KF_BT_CONNECTED; pending_save=1;
        timestamp=0; packet_bytes=packet_frames=send_pending=0;
        due_frames=due_remainder=last_ms=0;
        kf_audio_set_bt_route(1);
        btstack_run_loop_set_timer_handler(&audio_timer,timer_tick);
        btstack_run_loop_set_timer(&audio_timer,10);
        btstack_run_loop_add_timer(&audio_timer);
        break;
    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        send_packet(); break;
    case A2DP_SUBEVENT_STREAM_SUSPENDED:
    case A2DP_SUBEVENT_STREAM_RELEASED:
        stop_stream(); state=KF_BT_READY; break;
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        stop_stream(); a2dp_cid=0; state=KF_BT_READY; break;
    default: break;
    }
}
static void hci_event(uint8_t type,uint16_t channel,uint8_t *event,uint16_t size){
    (void)channel; (void)size;
    if(type!=HCI_EVENT_PACKET) return;
    switch(hci_event_packet_get_type(event)){
    case BTSTACK_EVENT_STATE:
        if(btstack_event_state_get_state(event)==HCI_STATE_WORKING){
            hci_ready=1;
            if(pending_connect){
                pending_connect=0;
                state=a2dp_source_establish_stream(target.addr,&a2dp_cid)==ERROR_CODE_SUCCESS ?
                    KF_BT_CONNECTING : KF_BT_FAILED;
            } else if(pending_scan){
                pending_scan=0;
                state=gap_inquiry_start(8)==ERROR_CODE_SUCCESS ? KF_BT_SCANNING : KF_BT_FAILED;
            } else state=KF_BT_READY;
        }
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:{
        bd_addr_t addr; hci_event_user_confirmation_request_get_bd_addr(event,addr);
        if(state==KF_BT_CONNECTING && !memcmp(addr,target.addr,6))
            gap_ssp_confirmation_response(addr);
        else gap_ssp_confirmation_negative(addr);
        break;
    }
    case HCI_EVENT_PIN_CODE_REQUEST:{
        bd_addr_t addr; hci_event_pin_code_request_get_bd_addr(event,addr);
        if(state==KF_BT_CONNECTING && !memcmp(addr,target.addr,6)) gap_pin_code_response(addr,"0000");
        else gap_pin_code_negative(addr);
        break;
    }
    case GAP_EVENT_INQUIRY_RESULT:{
        bd_addr_t addr; gap_event_inquiry_result_get_bd_addr(event,addr);
        uint32_t cod=gap_event_inquiry_result_get_class_of_device(event);
        /* Audio/Video major class or Rendering service. Keep the list useful
           instead of offering nearby phones, keyboards, and printers. */
        if((cod&0x1f00u)!=0x0400u && !(cod&0x040000u)) break;
        int i;
        for(i=0;i<scan_count;i++) if(!memcmp(scanned[i].addr,addr,6)) break;
        if(i==scan_count){ if(scan_count==BT_SCAN_MAX) break; scan_count++; }
        bt_device_t *d=&scanned[i]; memcpy(d->addr,addr,6);
        d->rssi=gap_event_inquiry_result_get_rssi_available(event) ?
            gap_event_inquiry_result_get_rssi(event) : -127;
        if(gap_event_inquiry_result_get_name_available(event)){
            int n=gap_event_inquiry_result_get_name_len(event);
            if(n>=(int)sizeof d->name) n=sizeof d->name-1;
            memcpy(d->name,gap_event_inquiry_result_get_name(event),n);
            d->name[n]=0;
        }
        if(!d->name[0]) address_text(addr,d->name);
        break;
    }
    case GAP_EVENT_INQUIRY_COMPLETE:
        if(state==KF_BT_SCANNING) state=KF_BT_READY;
        break;
    default: break;
    }
}
static int accept_incoming(bd_addr_t addr,hci_link_type_t link_type){
    (void)link_type;
    return state==KF_BT_CONNECTING && !memcmp(addr,target.addr,6);
}
int kf_bt_init(void){
    if(initialized) return 1;
    load_saved();
    kf_clock_eco(); eco_hold=1;
    kf_net_init(); /* initializes the one shared CYW43 context, including BTstack */
    if(!kf_net_present()){ state=KF_BT_FAILED; return 0; }
    hci_callback.callback=&hci_event;
    hci_add_event_handler(&hci_callback);
    gap_register_classic_connection_filter(accept_incoming);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_set_local_name("Kefyros PicoCalc");
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_discoverable_control(0);
    l2cap_init();
    a2dp_source_init();
    a2dp_source_register_packet_handler(a2dp_event);
    avdtp_stream_endpoint_t *sep=a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO,AVDTP_CODEC_SBC,codec_capabilities,sizeof codec_capabilities,
        codec_config,sizeof codec_config);
    if(!sep){ state=KF_BT_FAILED; return 0; }
    local_seid=avdtp_local_seid(sep);
    avdtp_set_preferred_sampling_frequency(sep,44100);
    sdp_init();
    a2dp_source_create_sdp_record(sdp_record,sdp_create_service_record_handle(),
                                   AVDTP_SOURCE_FEATURE_MASK_PLAYER,NULL,NULL);
    sdp_register_service(sdp_record);
    initialized=1;
    state=KF_BT_STARTING;
    if(hci_power_control(HCI_POWER_ON)!=ERROR_CODE_SUCCESS){
        state=KF_BT_FAILED; return 0;
    }
    return 1;
}
void kf_bt_poll(void){
    if(pending_save){ pending_save=0; remember_target(); }
    if(eco_hold && (state==KF_BT_CONNECTED || state==KF_BT_READY || state==KF_BT_FAILED)){
        eco_hold=0; kf_clock_normal();
    }
}
kf_bt_state_t kf_bt_state(void){ return state; }
const char *kf_bt_state_text(void){
    static const char *names[]={"off","starting","ready","scanning","connecting","connected","failed"};
    return names[state];
}
const char *kf_bt_device_name(void){ return target.name; }
int kf_bt_scan(void){
    if(!kf_bt_init() || state==KF_BT_CONNECTED || state==KF_BT_CONNECTING) return 0;
    if(state==KF_BT_SCANNING) return 1;
    scan_count=0; memset(scanned,0,sizeof scanned);
    kf_clock_eco(); eco_hold=1;
    if(!hci_ready){ pending_scan=1; state=KF_BT_SCANNING; return 1; }
    if(gap_inquiry_start(8)!=ERROR_CODE_SUCCESS){ state=KF_BT_FAILED; return 0; }
    state=KF_BT_SCANNING; return 1;
}
int kf_bt_scan_count(void){ return scan_count; }
const char *kf_bt_scan_name(int i){ return i>=0&&i<scan_count ? scanned[i].name : ""; }
const char *kf_bt_scan_address(int i){
    static char out[18];
    if(i<0||i>=scan_count) return "";
    address_text(scanned[i].addr,out); return out;
}
static int connect_device(const bt_device_t *d){
    if(!d || !kf_bt_init() || state==KF_BT_CONNECTED || state==KF_BT_CONNECTING) return 0;
    if(state==KF_BT_SCANNING){
        if(pending_scan) pending_scan=0;
        else gap_inquiry_stop();
    }
    target=*d;
    kf_clock_eco(); eco_hold=1;
    if(!hci_ready){ pending_connect=1; state=KF_BT_CONNECTING; return 1; }
    if(a2dp_source_establish_stream(target.addr,&a2dp_cid)!=ERROR_CODE_SUCCESS){
        state=KF_BT_FAILED; return 0;
    }
    state=KF_BT_CONNECTING; return 1;
}
int kf_bt_connect_scan(int i){ return i>=0&&i<scan_count ? connect_device(&scanned[i]) : 0; }
int kf_bt_saved_count(void){ load_saved(); return saved_count; }
const char *kf_bt_saved_name(int i){ load_saved(); return i>=0&&i<saved_count ? saved[i].name : ""; }
const char *kf_bt_saved_address(int i){
    static char out[18]; load_saved();
    if(i<0||i>=saved_count) return "";
    address_text(saved[i].addr,out); return out;
}
int kf_bt_connect_saved(int i){ load_saved(); return i>=0&&i<saved_count ? connect_device(&saved[i]) : 0; }
void kf_bt_forget_saved(int i){
    load_saved(); if(i<0||i>=saved_count) return;
    if(initialized) gap_drop_link_key_for_bd_addr(saved[i].addr);
    if(!memcmp(target.addr,saved[i].addr,6)) kf_bt_disconnect();
    for(int j=i+1;j<saved_count;j++) saved[j-1]=saved[j];
    saved_count--; persist_saved();
}
void kf_bt_disconnect(void){
    int active_scan=state==KF_BT_SCANNING && hci_ready && !pending_scan;
    pending_connect=pending_scan=0;
    if(active_scan) gap_inquiry_stop();
    stop_stream();
    if(a2dp_cid) a2dp_source_disconnect(a2dp_cid);
    a2dp_cid=0;
    state=initialized ? (hci_ready?KF_BT_READY:KF_BT_STARTING) : KF_BT_OFF;
}
