/* Real receiver + BTstack, fake transport; only caller's temporary DB is used. */
#include "btstack.h"
#include "btstack_run_loop_posix.h"
#include "pair_store.h"
#include "receiver.h"
#include <assert.h>
#include <stdio.h>
void pairing_test_working(void);
void pairing_test_drain(void);
static void (*receive_packet)(uint8_t,uint8_t *,uint16_t);
static uint16_t last_confirmation;
static void register_handler(void (*handler)(uint8_t,uint8_t *,uint16_t)){receive_packet=handler;}
static int send_packet(uint8_t type,uint8_t *packet,int size){
    if(type==HCI_COMMAND_DATA_PACKET&&size>=3){
        uint16_t opcode=little_endian_read_16(packet,0);
        if(opcode==0x042c||opcode==0x042d)last_confirmation=opcode;
    }
    return 0;
}
static const hci_transport_t transport={.name="fake-pairing-test",.register_packet_handler=register_handler,.send_packet=send_packet};
static bd_addr_t local={1,2,3,4,5,6},a={2,0,0,0,0,1},b={2,0,0,0,0,2};
static void request(bd_addr_t address){
    uint8_t packet[12]={HCI_EVENT_CONNECTION_REQUEST,10};reverse_bd_addr(address,packet+2);packet[11]=HCI_LINK_TYPE_ACL;
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
static void connected(bd_addr_t address,uint16_t handle){
    uint8_t packet[13]={HCI_EVENT_CONNECTION_COMPLETE,11};
    little_endian_store_16(packet,3,handle);reverse_bd_addr(address,packet+5);packet[11]=HCI_LINK_TYPE_ACL;
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
static void confirm(bd_addr_t address,uint16_t expected){
    pairing_test_working();last_confirmation=0;
    uint8_t packet[12]={HCI_EVENT_USER_CONFIRMATION_REQUEST,10};reverse_bd_addr(address,packet+2);
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
    pairing_test_drain();
    if(last_confirmation!=expected)fprintf(stderr,"confirmation: got %04x expected %04x\n",last_confirmation,expected);
    assert(last_confirmation==expected);
}
static void paired(bd_addr_t address,uint8_t status){
    uint8_t packet[9]={HCI_EVENT_SIMPLE_PAIRING_COMPLETE,7};packet[2]=status;reverse_bd_addr(address,packet+3);
    receive_packet(HCI_EVENT_PACKET,packet,sizeof(packet));
}
int main(int argc,char **argv){
    assert(argc==2);assert(pair_store_init(argv[1])==0);
    btstack_memory_init();btstack_run_loop_init(btstack_run_loop_posix_get_instance());hci_init(&transport,NULL);
    hci_set_link_key_db(pair_store_instance());pair_store_instance()->set_local_bd_addr(local);
    assert(receiver_init("synthetic speaker")==0);pairing_test_working();
    receiver_command("PAIRING 0");request(a);
    assert(!hci_connection_for_bd_addr_and_type(a,BD_ADDR_TYPE_ACL));
    receiver_command("PAIRING 1");request(a);connected(a,0x41);confirm(a,0x042c);
    paired(a,ERROR_CODE_AUTHENTICATION_FAILURE);assert(!pair_store_is_trusted(a));
    paired(a,0);assert(pair_store_is_trusted(a));assert(pair_store_count()==0); /* No reusable key. */
    receiver_command("PAIRING 0");confirm(a,0x042c);
    request(b);assert(!hci_connection_for_bd_addr_and_type(b,BD_ADDR_TYPE_ACL));
    assert(hci_connection_for_bd_addr_and_type(a,BD_ADDR_TYPE_ACL)->state!=SEND_DISCONNECT);
    uint8_t disconnected[6]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x41,0,0x13};
    receive_packet(HCI_EVENT_PACKET,disconnected,sizeof(disconnected));
    request(a);assert(hci_connection_for_bd_addr_and_type(a,BD_ADDR_TYPE_ACL));
    connected(a,0x43);confirm(a,0x042c);assert(pair_store_trusted_count()==1);
    /* Unknown peer admitted while on, but pairing closed before confirmation. */
    receiver_command("PAIRING 1");request(b);connected(b,0x42);
    receiver_command("PAIRING 0");confirm(b,0x042d);assert(!pair_store_is_trusted(b));
    pair_store_instance()->set_local_bd_addr(local);assert(pair_store_is_trusted(a));assert(!pair_store_is_trusted(b));
    receiver_shutdown();
    puts("PASS: unknown peer gated before takeover; failed SSP never trusted; successful no-bond SSP persisted; known peer confirms with pairing off; unknown confirmation rejected. Fake transport only.");
    return 0;
}
