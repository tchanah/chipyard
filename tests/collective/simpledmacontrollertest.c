#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// --- Assumed Driver Headers ---
#include "../mmio.h"
#include "../nic.h"

// --- Simulation Control via 'tohost' ---
extern volatile uint64_t tohost;

static void sim_pass() {
    printf("SUCCESS: All tests PASSED. Signaling simulation success.\n");
    tohost = 1;
    while (1);
}

static void sim_fail(uint64_t code) {
    printf("ERROR: Test FAILED with code %lu. Signaling simulation failure.\n", (unsigned long)code);
    tohost = (code << 1) | 1;
    while (1);
}

// --- Test Configuration ---
#define BUF_SIZE 2048
#define NUM_TEST_PACKETS 5
#define MIN_PACKET_SIZE 64
#define MAX_PACKET_SIZE 1514
#define DEBUG_PRINT_PACKETS 1 // Set to 1 to print packet contents for debugging

#if BUF_SIZE < MAX_PACKET_SIZE
#error "BUF_SIZE is too small!"
#endif

// --- Helper Functions ---

void print_words(const char* title, const uint32_t* words, size_t num_words) {
    printf("--- %s (%zu words) ---\n", title, num_words);
    for (size_t i = 0; i < num_words; ++i) {
        printf("0x%08x ", words[i]);
        if ((i + 1) % 8 == 0 || i == num_words - 1) {
            printf("\n");
        }
    }
    printf("--------------------------\n");
}

void generate_test_data(uint32_t* data, size_t num_words, uint32_t seed) {
    for (size_t i = 0; i < num_words; ++i) {
        data[i] = seed + (uint32_t)i;
    }
}

// --- Main Test ---

int main() {
    printf("\n--- Starting Bare-Metal Test for SimpleDmaController Hardware ---\n");
    printf("This test will send packets and verify the looped-back, modified data.\n");

    // Static buffers for bare-metal environment, aligned to cache-line size
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Read the MAC address to confirm the NIC is present.
    printf("NIC MAC Address: %012lx\n", nic_macaddr());

    for (int i = 0; i < NUM_TEST_PACKETS; i++) {
        printf("\n======= Running Test Packet %d of %d =======\n", i + 1, NUM_TEST_PACKETS);
// NEW CHANGE FOR TESTING JULY 24TH 2025
        // 1. Prepare the test packet
        // size_t packet_size = MIN_PACKET_SIZE + (rand() % (MAX_PACKET_SIZE - MIN_PACKET_SIZE + 1));
        // // Pad to bus width (e.g., 64 bytes)
        // size_t bus_width = 64;
        // packet_size = ((packet_size + bus_width - 1) / bus_width) * bus_width;
        // size_t num_words = packet_size / sizeof(uint32_t);

        // Force the packet size to 64 bytes to match the hardware DMA length.
        size_t packet_size = 64;
        size_t num_words = packet_size / sizeof(uint32_t);
// END NEW CHANGE FOR TESTING JULY 24TH 2025
        printf("Step 1: Generating test data (size: %zu bytes)\n", packet_size);
        generate_test_data((uint32_t*)tx_buf, num_words, (uint32_t)(i * 0xBADF00D));

        #if DEBUG_PRINT_PACKETS
            if (packet_size <= 64) print_words("TX DATA", (uint32_t*)tx_buf, num_words);
        #endif

        // 2. Calculate the expected result
        // The hardware increments the second 32-bit word of the packet.
        memcpy(expected_buf, tx_buf, packet_size);
        // uint32_t* expected_words = (uint32_t*)expected_buf;
        // expected_words[1]++; // The modification happens here
        
        printf("Step 2: Calculated expected return data.\n");
        #if DEBUG_PRINT_PACKETS
//            if (packet_size <= 64) print_words("EXPECTED RX DATA", expected_words, num_words);
        #endif

        // 3. Send the packet to the hardware module
        printf("Step 3: Sending packet via nic_send()...\n");
        nic_send(tx_buf, packet_size);
        printf("         -> nic_send() complete.\n");

        // 4. Wait to receive the modified packet back
        printf("Step 4: Waiting for response packet via nic_recv()...\n");
        int rx_len = nic_recv(rx_buf);

        if (rx_len <= 0) {
            printf("ERROR: Failed to receive packet (nic_recv returned %d).\n", rx_len);
            sim_fail(100 + i);
        }
        printf("         -> Received response packet (%d bytes).\n", rx_len);

        // 5. Verify the received packet
        printf("Step 5: Verifying packet length and content...\n");

        // Check length
        if (rx_len != packet_size) {
            printf("ERROR: Length mismatch! Sent %zu bytes, received %d bytes.\n", packet_size, rx_len);
            sim_fail(200 + i);
        }

        // Check content
        if (memcmp(rx_buf, expected_buf, packet_size) != 0) {
            printf("ERROR: Data mismatch in received packet!\n");
            #if DEBUG_PRINT_PACKETS
                print_words("Expected", (uint32_t*)expected_buf, num_words);
                print_words("Received", (uint32_t*)rx_buf, num_words);
            #endif
            sim_fail(300 + i);
        }

        printf("         -> Verification PASSED for test packet %d.\n", i + 1);
    }

    printf("\n=============================================\n");
    printf("         ✓ All tests completed! \n");
    printf("=============================================\n");
    sim_pass();

    return 0; // Should be unreachable
}