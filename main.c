#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "pair_store.h"
#include "receiver.h"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int IOBluetoothPreferenceGetControllerPowerState(void);
extern const hci_transport_t *hci_transport_skywalk_instance(void);
static btstack_packet_callback_registration_t events;
static btstack_timer_source_t startup_timer,ticker;
static btstack_data_source_t control;
static volatile sig_atomic_t interrupted;
static int ready;
static uint32_t last_ping;
static char speaker_name[256],commands[4096];
static size_t command_length;

static int find_speaker(void) {
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&size))return -1;
    AudioDeviceID *devices=malloc(size);if(!devices)return -1;
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&size,devices)){free(devices);return -1;}
    int found=-1;
    for(unsigned i=0;i<size/sizeof(*devices);i++) {
        AudioObjectPropertyAddress transport={kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 kind=0,length=sizeof(kind);
        if(AudioObjectGetPropertyData(devices[i],&transport,0,NULL,&length,&kind)||kind!=kAudioDeviceTransportTypeBuiltIn)continue;
        AudioObjectPropertyAddress streams={kAudioDevicePropertyStreams,kAudioDevicePropertyScopeOutput,kAudioObjectPropertyElementMain};
        length=0;if(AudioObjectGetPropertyDataSize(devices[i],&streams,0,NULL,&length)||!length)continue;
        AudioObjectPropertyAddress name_address={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        CFStringRef name=NULL;length=sizeof(name);
        if(!AudioObjectGetPropertyData(devices[i],&name_address,0,NULL,&length,&name)&&name) {
            if(CFStringGetCString(name,speaker_name,sizeof(speaker_name),kCFStringEncodingUTF8))found=0;
            CFRelease(name);
        }
        if(!found)break;
    }
    free(devices);return found;
}
static void state_event(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if(type!=HCI_EVENT_PACKET || size<3)return;
    if(packet[0]==BTSTACK_EVENT_STATE && btstack_event_state_get_state(packet)==HCI_STATE_WORKING) {
        if(!pair_store_ok()){btstack_run_loop_trigger_exit();return;}
        ready=1;btstack_run_loop_remove_timer(&startup_timer);
        puts("{\"event\":\"ready\",\"name\":\"Mac Switch Speaker\"}");receiver_status();
    } else if(packet[0]==BTSTACK_EVENT_POWERON_FAILED) {
        puts("{\"event\":\"error\",\"message\":\"控制器无法初始化，将恢复系统蓝牙\"}");btstack_run_loop_trigger_exit();
    } else if(packet[0]==HCI_EVENT_CONNECTION_COMPLETE && size>=5) {
        printf("LINK_CONNECTED handle=%04x status=%u\n",little_endian_read_16(packet,3),packet[2]);
    } else if(packet[0]==HCI_EVENT_DISCONNECTION_COMPLETE && size>=6) {
        printf("LINK_DISCONNECTED handle=%04x reason=%u\n",little_endian_read_16(packet,3),packet[5]);
    } else if(packet[0]==HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT && size>=5) {
        printf("LINK_AUTH handle=%04x status=%u\n",little_endian_read_16(packet,3),packet[2]);
    }
}
static void on_signal(int number){(void)number;interrupted=1;}
static void startup_timeout(btstack_timer_source_t *timer) {
    (void)timer;puts("{\"event\":\"error\",\"message\":\"蓝牙初始化超时，将恢复系统蓝牙\"}");btstack_run_loop_trigger_exit();
}
static void tick(btstack_timer_source_t *timer) {
    if(interrupted || (uint32_t)(btstack_run_loop_get_time_ms()-last_ping)>15000 || !pair_store_ok()) {
        btstack_run_loop_trigger_exit();return;
    }
    static unsigned ticks;
    if(++ticks==10){if(ready)receiver_status();ticks=0;}
    btstack_run_loop_set_timer(timer,100);btstack_run_loop_add_timer(timer);
}
static void control_input(btstack_data_source_t *source,btstack_data_source_callback_type_t type) {
    (void)source;(void)type;
    char buffer[1024];ssize_t length=read(STDIN_FILENO,buffer,sizeof(buffer));
    if(length==0){interrupted=1;return;}
    if(length<0){if(errno!=EAGAIN&&errno!=EINTR)interrupted=1;return;}
    for(ssize_t i=0;i<length;i++) {
        char c=buffer[i];
        if(c=='\n') {
            commands[command_length]=0;
            if(!strcmp(commands,"STOP"))interrupted=1;
            else if(!strcmp(commands,"PING"))last_ping=btstack_run_loop_get_time_ms();
            else receiver_command(commands);
            command_length=0;
        } else if(c!='\r' && command_length<sizeof(commands)-1)commands[command_length++]=c;
    }
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);umask(0077);
    if(argc==2&&!strcmp(argv[1],"--check")) {
        if(find_speaker())return 2;
        puts("{\"event\":\"check\",\"speaker\":true,\"durationLimit\":false}");return 0;
    }
    if(argc!=6||strcmp(argv[1],"--run")||strcmp(argv[2],"--state-dir")||strcmp(argv[4],"--session-token"))return 64;
    if(getuid()==0||IOBluetoothPreferenceGetControllerPowerState()!=0)return 65;
    if(pair_store_init(argv[3])||find_speaker()) {
        puts("{\"event\":\"error\",\"message\":\"无法打开应用数据目录或内置扬声器\"}");return 2;
    }
    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);
    btstack_memory_init();btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_init(hci_transport_skywalk_instance(),NULL);hci_set_link_key_db(pair_store_instance());
    events.callback=state_event;hci_add_event_handler(&events);
    if(receiver_init(speaker_name))return 3;
    fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);
    btstack_run_loop_set_data_source_fd(&control,STDIN_FILENO);
    btstack_run_loop_set_data_source_handler(&control,control_input);
    btstack_run_loop_enable_data_source_callbacks(&control,DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&control);
    btstack_run_loop_set_timer_handler(&startup_timer,startup_timeout);
    btstack_run_loop_set_timer(&startup_timer,15000);btstack_run_loop_add_timer(&startup_timer);
    btstack_run_loop_set_timer_handler(&ticker,tick);btstack_run_loop_set_timer(&ticker,100);btstack_run_loop_add_timer(&ticker);
    last_ping=btstack_run_loop_get_time_ms();hci_power_control(HCI_POWER_ON);btstack_run_loop_execute();
    hci_close();receiver_shutdown();
    puts("{\"event\":\"backend_stopped\"}");return ready?0:3;
}
