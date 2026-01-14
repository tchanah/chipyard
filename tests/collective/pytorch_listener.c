#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// --- MMIO and NIC Driver Headers ---
#include "../mmio.h" 
#include "../nic.h"

// --- Simulation Control via 'tohost' ---
extern volatile uint64_t tohost;

// Functions to signal simulation pass/fail
static inline void sim_pass() {
    printf("SUCCESS: Listener Running Forever.\n");
    fflush(stdout);
    // Do not exit
    while (1);
}

static inline void sim_fail(uint64_t code) {
    printf("ERROR: Test FAILED with code %lu.\n", (unsigned long)code);
    fflush(stdout);
    if (code == 0) code = 0xFF;
    tohost = (code << 1) | 1;
    while (1);
}

// --- Configuration ---
#define BUF_SIZE 2048
#define ETH_HEADER_LEN 16
#define TOTAL_PACKET_LEN (ETH_HEADER_LEN + 16 + 1024) // 1056

#define BASE_MAC 0x00126D000000ULL
#define TESTER_MAC_OFFSET 0x02
#define ACCELERATOR_MAC_OFFSET 0x22

#define META_OP_SETUP     0xFE

// MAC Helper
#define WRITE_MAC_TO_BUF(buf, offset, mac) do { \
    for (int _i = 0; _i < 6; _i++) { \
        (buf)[(offset) + _i] = (uint8_t)(((mac) >> ((5 - _i) * 8)) & 0xFF); \
    } \
} while(0)

// Rank Definition
#ifndef NODE_RANK
#define NODE_RANK 0
#endif

// --- Setup Helper ---
int send_setup_packet(int rank) {
   printf("Sending Setup+Warmup Packet to accelerator (Setting Rank=%d)...\n", rank);
   
   static uint8_t tx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
   static uint8_t rx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
   
   uint64_t tester_mac_for_setup = BASE_MAC | ((uint64_t)(rank + TESTER_MAC_OFFSET));
   uint64_t accel_mac_for_setup = BASE_MAC | ((uint64_t)(rank + ACCELERATOR_MAC_OFFSET));

   memset(tx_buf_setup, 0, TOTAL_PACKET_LEN);
   
   // Ethernet header
   tx_buf_setup[0] = 0x00; tx_buf_setup[1] = 0x00;
   WRITE_MAC_TO_BUF(tx_buf_setup, 2, accel_mac_for_setup);
   WRITE_MAC_TO_BUF(tx_buf_setup, 8, tester_mac_for_setup);
   tx_buf_setup[14] = 0x00; tx_buf_setup[15] = 0x00;
   
   // Metadata
   uint8_t *meta = tx_buf_setup + ETH_HEADER_LEN;
   meta[0] = 0xFF; meta[1] = 0xFF; // CollID = 0xFFFF (ACK trigger)
   meta[2] = 0;
   meta[3] = META_OP_SETUP;        // 0xFE
   meta[5] = (uint8_t)rank;        // Rank Config
   
   reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
   nic_send(tx_buf_setup, TOTAL_PACKET_LEN);
   
   printf("Waiting for Setup ACK...\n");
   uint64_t timeout = 20000000;
   while (timeout > 0) {
       if (nic_recv_comp_avail() > 0) {
           reg_read16(SIMPLENIC_RECV_COMP);
           asm volatile ("fence");
           
           uint8_t *rx_meta = rx_buf_setup + ETH_HEADER_LEN;
           uint16_t coll_id = rx_meta[0] | (rx_meta[1] << 8);
           uint8_t op_code = rx_meta[3];
           
           if (coll_id == 0xFFFF && op_code == META_OP_SETUP) {
               printf("Setup ACK received! Rank=%d confirmed.\n", rank);
               return 1;
           } else {
               printf("Ignoring non-ACK packet during setup...\n");
               reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
           }
       }
       timeout--;
   }
   printf("ERROR: Setup ACK timeout!\n");
   return 0;
}

// --- Main ---
int main(int argc, char *argv[]) {
    (void)nic_recv; // suppress unused

    printf("Starting PyTorch Listener Node Rank %d...\n", NODE_RANK);

    uint8_t TEST_NODE_RANK = NODE_RANK;
    uint64_t tester_mac = BASE_MAC | ((uint64_t)(TEST_NODE_RANK + TESTER_MAC_OFFSET));

    printf("Programming NIC MAC: %012lx\n", (unsigned long)tester_mac);
    reg_write64(SIMPLENIC_MACADDR, tester_mac);
    asm volatile ("fence");

    // Perform Setup
    if (!send_setup_packet(TEST_NODE_RANK)) {
        sim_fail(1);
    }

    printf("Setup Complete. Entering Passive Listen Mode.\n");
    printf("This node will now sink packets sent by PyTorch (via Switch -> Accel -> Ack -> Here).\n");

    // Recv Buffers
    #define NUM_RX_BUFFERS 16
    static uint8_t rx_buffers[NUM_RX_BUFFERS][BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t *rx_buf = NULL;
    static int rx_tail = 0;

    // Post buffers
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        while (nic_recv_req_avail() == 0);
        reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buffers[i]);
    }

    uint64_t packet_count = 0;

    while (1) {
        if (nic_recv_comp_avail() > 0) {
            rx_buf = rx_buffers[rx_tail];
            rx_tail = (rx_tail + 1) % NUM_RX_BUFFERS;

            int len = reg_read16(SIMPLENIC_RECV_COMP);
            asm volatile ("fence");

            packet_count++;
            if (packet_count % 100 == 0) {
                 printf("Received %lu packets so far. Last len: %d\n", packet_count, len);
            }

            // Post back
            memset(rx_buf, 0, BUF_SIZE); 
            while (nic_recv_req_avail() == 0);
            reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
        }
    }

    return 0;
}
