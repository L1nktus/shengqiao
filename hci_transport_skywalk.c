#include "btstack.h"
#include "native_channel.h"
#include <stdio.h>
#include <string.h>

static native_channel *hci, *acl;
static btstack_data_source_t hci_source, acl_source;
static void (*packet_handler)(uint8_t, uint8_t *, uint16_t);
static int fault;
static uint8_t storage[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_ACL_PAYLOAD_SIZE + 8];
static void trace_signal(const char *direction,const uint8_t *packet,int size) {
    if(size<8 || little_endian_read_16(packet,6)!=1 || (packet[1]&0x30)==0x10)return;
    printf("L2CAP_%s_SIGNAL handle=%04x bytes=%d data=",direction,little_endian_read_16(packet,0)&0x0fff,size);
    for(int i=8;i<size && i<56;i++)printf("%02x",packet[i]);puts("");
}

static void receive_data(btstack_data_source_t *source, btstack_data_source_callback_type_t callback) {
    (void)callback;
    native_channel *channel = source == &hci_source ? hci : acl;
    uint8_t type = source == &hci_source ? HCI_EVENT_PACKET : HCI_ACL_DATA_PACKET;
    for (int batch = 0; batch < 16; batch++) {
        uint8_t *packet = storage + HCI_INCOMING_PRE_BUFFER_SIZE;
        int size = native_channel_receive(channel, packet, sizeof(storage) - HCI_INCOMING_PRE_BUFFER_SIZE);
        if (size == 0) break;
        if (size < 0 || (type == HCI_EVENT_PACKET &&
            (size < 2 || packet[1] + 2 != size)) ||
            (type == HCI_ACL_DATA_PACKET &&
            (size < 4 || little_endian_read_16(packet, 2) + 4 != size))) {
            fprintf(stderr, "NATIVE_RX_INVALID type=%u size=%d\n", type, size);
            fault = 1; btstack_run_loop_trigger_exit(); break;
        }
        if (type == HCI_EVENT_PACKET && packet[0] == HCI_EVENT_COMMAND_COMPLETE && size >= 6)
            printf("HCI_COMPLETE opcode=%04x status=%u\n", little_endian_read_16(packet, 3), packet[5]);
        if (type == HCI_ACL_DATA_PACKET) trace_signal("RX",packet,size);
        packet_handler(type, packet, (uint16_t)size);
    }
}

static void init(const void *config) { (void)config; fault = 0; }

static void add_source(btstack_data_source_t *source, native_channel *channel) {
    memset(source, 0, sizeof(*source));
    btstack_run_loop_set_data_source_fd(source, native_channel_fd(channel));
    btstack_run_loop_set_data_source_handler(source, receive_data);
    btstack_run_loop_enable_data_source_callbacks(source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(source);
}

static int open_transport(void) {
    hci = native_channel_open("hci");
    if (!hci) return -1;
    acl = native_channel_open("acl");
    if (!acl) { native_channel_close(hci); hci = NULL; return -1; }
    add_source(&hci_source, hci); add_source(&acl_source, acl);
    return 0;
}

static int close_transport(void) {
    if (hci) { btstack_run_loop_remove_data_source(&hci_source); native_channel_close(hci); hci = NULL; }
    if (acl) { btstack_run_loop_remove_data_source(&acl_source); native_channel_close(acl); acl = NULL; }
    return 0;
}

static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t)) { packet_handler = handler; }

static int send_packet(uint8_t type, uint8_t *packet, int size) {
    if (fault || size < 0) return -1;
    native_channel *channel;
    if (type == HCI_COMMAND_DATA_PACKET) {
        if (size < 3 || packet[2] + 3 != size) return -1;
        uint16_t opcode = little_endian_read_16(packet, 0);
        /* No vendor commands, firmware loading, address spoofing or NVRAM access. */
        if ((opcode >> 10) == 0x3f) {
            fprintf(stderr, "BLOCKED_VENDOR_OPCODE %04x\n", opcode);
            fault = 1; btstack_run_loop_trigger_exit(); return -1;
        }
        printf("HCI_TX opcode=%04x bytes=%d\n", opcode, size);
        channel = hci;
    } else if (type == HCI_ACL_DATA_PACKET) {
        trace_signal("TX",packet,size);
        channel = acl;
    } else return -1;
    if (!channel || native_channel_send(channel, packet, (size_t)size)) {
        fprintf(stderr, "NATIVE_TX_FAILED type=%u size=%d\n", type, size);
        fault = 1; btstack_run_loop_trigger_exit(); return -1;
    }
    return 0;
}

const hci_transport_t *hci_transport_skywalk_instance(void) {
    static const hci_transport_t transport = {
        .name = "Apple PCIe Skywalk", .init = init, .open = open_transport,
        .close = close_transport, .register_packet_handler = register_handler,
        .can_send_packet_now = NULL, .send_packet = send_packet
    };
    return &transport;
}
