/* Test-only access to BTstack state: compile the unmodified implementation in
 * this translation unit. Production compiles vendor hci.c directly. No radio. */
#include "vendor/btstack/src/hci.c"
void pairing_test_working(void) {
    /* Isolate pairing-command scheduling from unrelated fake controller setup.
     * Release the previous fake command buffer as a controller ack would. */
    hci_stack->state=HCI_STATE_WORKING;
    hci_stack->gap_pairing_state=GAP_PAIRING_STATE_IDLE;
    hci_stack->num_cmd_packets=255;
    hci_stack->hci_packet_buffer_reserved=false;
    hci_stack->gap_tasks_classic=0;
}

void pairing_test_drain(void) {
    for(unsigned i=0;i<32;i++) {
        hci_stack->num_cmd_packets=255;
        hci_stack->hci_packet_buffer_reserved=false;
        hci_stack->gap_tasks_classic=0;
        hci_run();
    }
}
