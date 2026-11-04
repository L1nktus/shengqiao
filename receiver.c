#include "btstack.h"
#include "mixer.h"
#include "pair_store.h"
#include "receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct peer {
    struct peer *next;
    uint16_t cid, avrcp;
    bd_addr_t address;
    unsigned volume, remote;
} peer;
typedef struct endpoint {
    struct endpoint *next;
    uint8_t seid, configuration[4];
    uint16_t cid;
    unsigned rate, playing;
    avdtp_stream_endpoint_t *endpoint;
    btstack_sbc_decoder_bluedroid_t decoder;
    const btstack_sbc_decoder_t *codec;
    mixer_source *source;
    uint8_t fragment[4096];
    size_t fragment_size;
    uint16_t fragment_sequence;
} endpoint;
static peer *peers;
static endpoint *endpoints;
static bd_addr_t requested_peer;
static int requested_peer_valid;
static uint16_t active_cid;
static const char *speaker_name;
static int audio_open, accepting_pairing = 1;
static unsigned master_volume = 100, gain = 100;
static btstack_packet_callback_registration_t pairing_events;
static uint8_t sdp_sink[200], sdp_controller[200], sdp_target[200], inquiry[240];
static const uint8_t capabilities[4] = {0xff, 0xff, 2, 53};

static peer *peer_for_cid(uint16_t cid) {
    for (peer *p=peers; p; p=p->next) if (p->cid==cid) return p;
    return NULL;
}
static peer *peer_for_avrcp(uint16_t cid) {
    for (peer *p=peers; p; p=p->next) if (p->avrcp==cid) return p;
    return NULL;
}
static endpoint *endpoint_for_seid(uint8_t seid) {
    for (endpoint *e=endpoints; e; e=e->next) if (e->seid==seid) return e;
    return NULL;
}
static endpoint *endpoint_for_cid(uint16_t cid) {
    for (endpoint *e=endpoints; e; e=e->next) if (e->cid==cid) return e;
    return NULL;
}
static void sync_volume(peer *p) {
    if (!p) return;
    endpoint *e=endpoint_for_cid(p->cid);
    if (e && e->source) mixer_source_volume(e->source,p->volume,p->remote);
}
static void pcm(int16_t *data,int frames,int channels,int rate,void *context) {
    endpoint *e=context;
    if (e->source && e->playing && rate==(int)e->rate)
        mixer_source_push(e->source,data,(size_t)frames,(unsigned)channels);
}
static void close_unused_audio(void);
/* A new peer takes over. Do not reject it because the previous ACL still exists:
 * controller teardown is asynchronous, but old PCM stops immediately. */
static int accept_and_take_over(bd_addr_t address, hci_link_type_t type) {
    if (type != HCI_LINK_TYPE_ACL) return 0;
    if (!accepting_pairing && !pair_store_is_trusted(address)) return 0;
    if (requested_peer_valid && !bd_addr_cmp(requested_peer,address)) return 1;
    bd_addr_copy(requested_peer,address);requested_peer_valid=1;active_cid=0;
    for (endpoint *e=endpoints;e;e=e->next) {
        e->playing=0;e->fragment_size=0;
        if(e->source)mixer_source_playing(e->source,0);
    }
    close_unused_audio();
    btstack_linked_list_iterator_t it;
    hci_connections_get_iterator(&it);
    while (btstack_linked_list_iterator_has_next(&it)) {
        hci_connection_t *connection=(hci_connection_t *)btstack_linked_list_iterator_next(&it);
        if (connection->address_type==BD_ADDR_TYPE_ACL && bd_addr_cmp(connection->address,address)) {
            if(connection->con_handle!=HCI_CON_HANDLE_INVALID)gap_disconnect(connection->con_handle);
        }
    }
    return 1;
}
static int create_endpoint(void) {
    for(endpoint *e=endpoints;e;e=e->next)if(!e->cid&&!e->endpoint->sep.in_use)return 0;
    {
        endpoint *e=calloc(1,sizeof(*e));
        if (!e) return -1;
        e->endpoint=a2dp_sink_create_stream_endpoint(AVDTP_AUDIO,AVDTP_CODEC_SBC,
            capabilities,sizeof(capabilities),e->configuration,sizeof(e->configuration));
        if (!e->endpoint) { free(e); return -1; }
        e->seid=avdtp_local_seid(e->endpoint);
        e->next=endpoints; endpoints=e;
    }
    return 0;
}
static void close_unused_audio(void) {
    for (endpoint *e=endpoints;e;e=e->next) if (e->playing) return;
    if (audio_open) { mixer_audio_close(); audio_open=0; }
}
static void reset_endpoint(endpoint *e) {
    if (!e) return;
    e->playing=0; e->cid=0; e->fragment_size=0;
    mixer_source_destroy(e->source); e->source=NULL;
    close_unused_audio();
}
void receiver_status(void) {
    printf("{\"event\":\"sources\",\"paired\":%zu,\"clipped\":%llu,\"outputMs\":%.1f,\"outputUnderruns\":%llu,\"outputContentions\":%llu,\"sources\":[",
        pair_store_trusted_count(),(unsigned long long)mixer_clipped_samples(),mixer_audio_latency_ms(),
        (unsigned long long)mixer_audio_underruns(),(unsigned long long)mixer_audio_contentions());
    int comma=0;
    for (peer *p=peers;p;p=p->next) {
        if(p->cid!=active_cid)continue;
        endpoint *e=endpoint_for_cid(p->cid);
        mixer_source_statistics statistics;
        mixer_source_get_statistics(e?e->source:NULL,&statistics);
        printf("%s{\"id\":%u,\"address\":\"%s\",\"playing\":%s,\"rate\":%u,\"volume\":%u,\"remote\":%u,\"bufferMs\":%.1f,\"peakBufferMs\":%.1f,\"underruns\":%llu,\"droppedFrames\":%llu,\"targetMs\":%u}",
            comma++?",":"",p->cid,bd_addr_to_str(p->address),e&&e->playing?"true":"false",e?e->rate:0,p->volume,p->remote,
            statistics.queued_ms,statistics.peak_queued_ms,(unsigned long long)statistics.underruns,
            (unsigned long long)statistics.dropped_frames,statistics.target_ms);
    }
    puts("]}");
}

static void media(uint8_t seid,uint8_t *packet,uint16_t size) {
    endpoint *e=endpoint_for_seid(seid);
    if (!e || e->cid!=active_cid || !e->source || !e->playing || size<13 || (packet[0]>>6)!=2) return;
    size_t offset=12+4*(packet[0]&15),end=size;
    if (offset>=end) return;
    if (packet[0]&0x10) {
        if (offset+4>end) return;
        offset+=4+4*(size_t)big_endian_read_16(packet,(int)offset+2);
    }
    if (packet[0]&0x20) {
        unsigned padding=packet[end-1]; if (!padding || padding>end-offset) return; end-=padding;
    }
    if (offset>=end) return;
    uint8_t header=packet[offset++];
    size_t bytes=end-offset;
    if (header&0x80) {
        uint16_t sequence=big_endian_read_16(packet,2);
        if (header&0x40) e->fragment_size=0;
        else if (!e->fragment_size || sequence!=(uint16_t)(e->fragment_sequence+1)) { e->fragment_size=0; return; }
        if (e->fragment_size+bytes>sizeof(e->fragment)) { e->fragment_size=0; return; }
        memcpy(e->fragment+e->fragment_size,packet+offset,bytes); e->fragment_size+=bytes;
        e->fragment_sequence=sequence;
        if (header&0x20) {
            e->codec->decode_signed_16(&e->decoder,0,e->fragment,(uint16_t)e->fragment_size); e->fragment_size=0;
        }
    } else if ((header&15) && bytes) {
        e->codec->decode_signed_16(&e->decoder,0,packet+offset,(uint16_t)bytes);
    }
}

static void a2dp_events(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if (type!=HCI_EVENT_PACKET || size<3 || packet[0]!=HCI_EVENT_A2DP_META) return;
    uint16_t cid; endpoint *e;
    switch (packet[2]) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            if (a2dp_subevent_signaling_connection_established_get_status(packet)) break;
            cid=a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            bd_addr_t address;
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet,address);
            if(requested_peer_valid && bd_addr_cmp(address,requested_peer)) { a2dp_sink_disconnect(cid);break; }
            peer *p=peer_for_cid(cid);
            if (!p) { p=calloc(1,sizeof(*p)); if (!p) break; p->cid=cid;p->volume=100;p->remote=127;p->next=peers;peers=p; }
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet,p->address);
            active_cid=cid;
            if(create_endpoint()) { a2dp_sink_disconnect(cid);active_cid=0; }
            receiver_status(); break;
        }
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            cid=a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
            if(cid!=active_cid)break;
            e=endpoint_for_seid(a2dp_subevent_signaling_media_codec_sbc_configuration_get_local_seid(packet));
            if (!e) break;
            e->cid=cid;e->rate=a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            if (e->source) mixer_source_rate(e->source,e->rate);
            else e->source=mixer_source_create(cid,e->rate);
            e->codec=btstack_sbc_decoder_bluedroid_init_instance(&e->decoder);
            e->codec->configure(&e->decoder,SBC_MODE_STANDARD,pcm,e);
            sync_volume(peer_for_cid(cid));
            receiver_status(); break;
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            if (a2dp_subevent_stream_established_get_status(packet))
                puts("{\"event\":\"notice\",\"message\":\"一个音源的媒体连接未成功\"}");
            break;
        case A2DP_SUBEVENT_STREAM_STARTED:
            e=endpoint_for_seid(a2dp_subevent_stream_started_get_local_seid(packet));
            if (!e || e->cid!=active_cid || !e->source) break;
            e->playing=1;mixer_source_playing(e->source,1);
            if (!audio_open) {
                int result=mixer_audio_open(speaker_name);
                if (result) { puts("{\"event\":\"error\",\"message\":\"内置扬声器无法打开\"}");btstack_run_loop_trigger_exit();break; }
                audio_open=1;
            }
            receiver_status(); break;
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            e=endpoint_for_seid(a2dp_subevent_stream_suspended_get_local_seid(packet));
            if (e) { e->playing=0;mixer_source_playing(e->source,0); }
            close_unused_audio();receiver_status();break;
        case A2DP_SUBEVENT_STREAM_RELEASED:
            reset_endpoint(endpoint_for_seid(a2dp_subevent_stream_released_get_local_seid(packet)));
            receiver_status();break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED: {
            cid=a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet);
            if(cid==active_cid)active_cid=0;
            reset_endpoint(endpoint_for_cid(cid));
            peer **p=&peers;while(*p && (*p)->cid!=cid)p=&(*p)->next;
            if (*p) { peer *removed=*p;*p=removed->next;free(removed); }
            receiver_status();break;
        }
        default:break;
    }
}

static void avrcp_events(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if(type!=HCI_EVENT_PACKET || size<3 || packet[0]!=HCI_EVENT_AVRCP_META)return;
    if(packet[2]==AVRCP_SUBEVENT_CONNECTION_ESTABLISHED) {
        if(avrcp_subevent_connection_established_get_status(packet))return;
        bd_addr_t address;avrcp_subevent_connection_established_get_bd_addr(packet,address);
        for(peer *p=peers;p;p=p->next)if(!bd_addr_cmp(address,p->address)) {
            p->avrcp=avrcp_subevent_connection_established_get_avrcp_cid(packet);
            avrcp_target_support_event(p->avrcp,AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);break;
        }
    } else if(packet[2]==AVRCP_SUBEVENT_CONNECTION_RELEASED) {
        peer *p=peer_for_avrcp(avrcp_subevent_connection_released_get_avrcp_cid(packet));if(p)p->avrcp=0;
    }
}
static void target_events(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if(type!=HCI_EVENT_PACKET || size<3 || packet[0]!=HCI_EVENT_AVRCP_META)return;
    peer *p=NULL;
    if(packet[2]==AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED) {
        p=peer_for_avrcp(avrcp_subevent_notification_volume_changed_get_avrcp_cid(packet));
        if(p)p->remote=avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
    } else if(packet[2]==AVRCP_SUBEVENT_OPERATION && avrcp_subevent_operation_get_button_pressed(packet)) {
        p=peer_for_avrcp(avrcp_subevent_operation_get_avrcp_cid(packet));
        if(p) {
            unsigned op=avrcp_subevent_operation_get_operation_id(packet);
            if(op==AVRCP_OPERATION_ID_VOLUME_UP)p->remote=p->remote>114?127:p->remote+13;
            else if(op==AVRCP_OPERATION_ID_VOLUME_DOWN)p->remote=p->remote<13?0:p->remote-13;
            avrcp_target_volume_changed(p->avrcp,(uint8_t)p->remote);
        }
    }
    if(p){sync_volume(p);receiver_status();}
}
static void controller_events(uint8_t t,uint16_t c,uint8_t *p,uint16_t s){(void)t;(void)c;(void)p;(void)s;}
static void pairing_handler(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if(type!=HCI_EVENT_PACKET || size<2)return;
    bd_addr_t address;
    if(packet[0]==HCI_EVENT_CONNECTION_COMPLETE && size>=13 && packet[2]==0 &&
       hci_event_connection_complete_get_link_type(packet)==HCI_LINK_TYPE_ACL) {
        hci_event_connection_complete_get_bd_addr(packet,address);
        if(requested_peer_valid && bd_addr_cmp(address,requested_peer))
            gap_disconnect(little_endian_read_16(packet,3));
    } else if(packet[0]==HCI_EVENT_USER_CONFIRMATION_REQUEST && size>=12) {
        hci_event_user_confirmation_request_get_bd_addr(packet,address);
        if(accepting_pairing || pair_store_is_trusted(address))gap_ssp_confirmation_response(address);else gap_ssp_confirmation_negative(address);
    } else if(packet[0]==HCI_EVENT_SIMPLE_PAIRING_COMPLETE && size>=9 && packet[2]==0) {
        hci_event_simple_pairing_complete_get_bd_addr(packet,address);
        if(hci_connection_for_bd_addr_and_type(address,BD_ADDR_TYPE_ACL) &&
           (accepting_pairing || pair_store_is_trusted(address)))pair_store_trust(address);
    } else if(packet[0]==HCI_EVENT_PIN_CODE_REQUEST) {
        hci_event_pin_code_request_get_bd_addr(packet,address);gap_pin_code_negative(address);
    }
}

int receiver_init(const char *speaker) {
    gap_register_classic_connection_filter(accept_and_take_over);
    speaker_name=speaker;l2cap_init();sdp_init();a2dp_sink_init();avrcp_init();avrcp_controller_init();avrcp_target_init();
    a2dp_sink_register_packet_handler(a2dp_events);a2dp_sink_register_media_handler(media);
    avrcp_register_packet_handler(avrcp_events);avrcp_controller_register_packet_handler(controller_events);
    avrcp_target_register_packet_handler(target_events);
    if(create_endpoint())return -1;
    a2dp_sink_create_sdp_record(sdp_sink,sdp_create_service_record_handle(),AVDTP_SINK_FEATURE_MASK_SPEAKER,"Mac Switch Speaker",NULL);
    sdp_register_service(sdp_sink);
    avrcp_controller_create_sdp_record(sdp_controller,sdp_create_service_record_handle(),1,"Audio Controller",NULL);
    sdp_register_service(sdp_controller);
    avrcp_target_create_sdp_record(sdp_target,sdp_create_service_record_handle(),2,"Audio Volume",NULL);
    sdp_register_service(sdp_target);
    const uint8_t uuids[]={9,3,0x0b,0x11,0x0c,0x11,0x0e,0x11,0x0f,0x11};
    const char *name="Mac Switch Speaker";memcpy(inquiry,uuids,sizeof(uuids));
    inquiry[sizeof(uuids)]=(uint8_t)strlen(name)+1;inquiry[sizeof(uuids)+1]=9;
    memcpy(inquiry+sizeof(uuids)+2,name,strlen(name));gap_set_extended_inquiry_response(inquiry);
    gap_set_local_name(name);gap_set_class_of_device(0x240414);gap_discoverable_control(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH|LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_ssp_set_auto_accept(0);
    gap_set_allow_role_switch(true);gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    pairing_events.callback=pairing_handler;hci_add_event_handler(&pairing_events);
    return 0;
}
void receiver_command(const char *command) {
    unsigned value,id;
    if(!strcmp(command,"FLUSH")) {
        mixer_flush();
        for(endpoint *e=endpoints;e;e=e->next)e->fragment_size=0;
        puts("{\"event\":\"buffer_refreshed\"}");receiver_status();
    } else if(sscanf(command,"MASTER %u",&value)==1 && value<=100)master_volume=value;
    else if(sscanf(command,"GAIN %u",&value)==1 && value>=100 && value<=400)gain=value;
    else if(sscanf(command,"SOURCE %u %u",&id,&value)==2 && value<=100) {
        peer *p=peer_for_cid((uint16_t)id);if(p){p->volume=value;sync_volume(p);}
    } else if(sscanf(command,"PAIRING %u",&value)==1 && value<=1) {
        accepting_pairing=(int)value;gap_discoverable_control(accepting_pairing);
    }
    mixer_set_master(master_volume,gain);
}
void receiver_shutdown(void) {
    mixer_audio_close();audio_open=0;
    for(endpoint *e=endpoints;e;) { endpoint *next=e->next;mixer_source_destroy(e->source);free(e);e=next; }endpoints=NULL;
    for(peer *p=peers;p;) { peer *next=p->next;free(p);p=next; }peers=NULL;
}
