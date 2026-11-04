/* Exercise the real receiver and BTstack with synthetic HCI events only.
 * No native transport, controller APIs, audio devices or pairing DB are opened. */
#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void (*receive_packet)(uint8_t,uint8_t *,uint16_t);
static void register_handler(void (*handler)(uint8_t,uint8_t *,uint16_t)){receive_packet=handler;}
static int send_packet(uint8_t type,uint8_t *packet,int size){(void)type;(void)packet;(void)size;return 0;}
static const hci_transport_t transport={.name="fake-policy-test",.register_packet_handler=register_handler,.send_packet=send_packet};
static const bd_addr_t a={2,0,0,0,0,1},b={2,0,0,0,0,2};
static unsigned connections(void){
    unsigned count=0;btstack_linked_list_iterator_t it;hci_connections_get_iterator(&it);
    while(btstack_linked_list_iterator_has_next(&it)){btstack_linked_list_iterator_next(&it);count++;}return count;
}
static void request(const bd_addr_t address,uint8_t type){
    uint8_t packet[12]={HCI_EVENT_CONNECTION_REQUEST,10};reverse_bd_addr(address,packet+2);packet[11]=type;
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
static void complete(const bd_addr_t address,uint8_t status,uint16_t handle){
    uint8_t packet[13]={HCI_EVENT_CONNECTION_COMPLETE,11};packet[2]=status;
    little_endian_store_16(packet,3,handle);reverse_bd_addr(address,packet+5);packet[11]=HCI_LINK_TYPE_ACL;
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
static void disconnect(uint16_t handle){
    uint8_t packet[6]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0,0,0x13};little_endian_store_16(packet,3,handle);
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
int main(void){
    btstack_memory_init();btstack_run_loop_init(btstack_run_loop_posix_get_instance());hci_init(&transport,NULL);
    assert(receive_packet);assert(receiver_init("synthetic speaker")==0);
    assert(btstack_linked_list_count(avdtp_get_stream_endpoints())==1);
    request(a,HCI_LINK_TYPE_SCO);assert(connections()==0);
    request(a,HCI_LINK_TYPE_ACL);complete(a,0,0x0041);assert(connections()==1);
    request(b,HCI_LINK_TYPE_ACL);assert(connections()==2);
    assert(hci_connection_for_bd_addr_and_type(a,BD_ADDR_TYPE_ACL)->state==SEND_DISCONNECT);
    complete(b,0,0x0042);disconnect(0x0041);assert(connections()==1);
    assert(hci_connection_for_bd_addr_and_type(b,BD_ADDR_TYPE_ACL)!=NULL);
    /* Switch back entirely from the source device, without Mac interaction. */
    request(a,HCI_LINK_TYPE_ACL);assert(connections()==2);
    assert(hci_connection_for_bd_addr_and_type(b,BD_ADDR_TYPE_ACL)->state==SEND_DISCONNECT);
    complete(a,0,0x0043);disconnect(0x0042);assert(connections()==1);
    disconnect(0x0043);assert(connections()==0);
    /* Latest request wins even while an earlier peer is still connecting. */
    request(a,HCI_LINK_TYPE_ACL);request(b,HCI_LINK_TYPE_ACL);assert(connections()==2);
    complete(a,0,0x0044);
    assert(hci_connection_for_bd_addr_and_type(a,BD_ADDR_TYPE_ACL)->state==SEND_DISCONNECT);
    complete(b,0,0x0045);disconnect(0x0044);assert(connections()==1);
    disconnect(0x0045);assert(connections()==0);
    request(a,HCI_LINK_TYPE_ACL);complete(a,ERROR_CODE_AUTHENTICATION_FAILURE,0);assert(connections()==0);
    request(b,HCI_LINK_TYPE_ACL);complete(b,0,0x0046);assert(connections()==1);
    disconnect(0x0046);assert(connections()==0);
    receiver_shutdown();
    puts("PASS: new audio peer is accepted and automatically disconnects old peer; switch back without Mac interaction; latest pending request wins; failed connection can retry. No Bluetooth hardware used.");
    return 0;
}
