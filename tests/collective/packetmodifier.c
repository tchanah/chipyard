#include <stdio.h>  // Keep for printf
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // Keep for NULL, size_t etc. if needed by string.h, but not exit()

// --- MMIO and NIC Driver Headers ---
#include "../mmio.h"
#include "../nic.h"

// --- Simulation Control via 'tohost' ---

// IMPORTANT: 'tohost' address must be provided by your linker script!
// The simulation harness monitors this address.
extern volatile uint64_t tohost;

// Function to signal successful test completion to the simulator
static inline void sim_pass() {
    // Optional: Final success printf before exiting
    printf("SUCCESS: Test PASSED. Signaling simulation success.\n");
    // Flush stdio buffer if necessary/possible in your environment
    // fflush(stdout);
    tohost = 1; // Standard encoding for success (exit code 0)
    while (1);  // Loop forever after signaling
}

// Function to signal failed test completion to the simulator
// Use different non-zero codes for different failure points
static inline void sim_fail(uint64_t code) {
    // Optional: Final error printf before exiting
    printf("ERROR: Test FAILED with code %lu. Signaling simulation failure.\n", (unsigned long)code);
    // Flush stdio buffer if necessary/possible in your environment
    // fflush(stdout);
    if (code == 0) code = 0xFF; // Ensure failure code is non-zero
    tohost = (code << 1) | 1; // Standard encoding for failure
    while (1); // Loop forever after signaling
}


// --- Test Configuration ---
#define BUF_SIZE 2048
#define TEST_PKT_LEN 32

// --- Helper Functions ---

// Simple function to print a buffer in hex (Uses printf)
void print_buf(const char* title, const uint8_t* buf, size_t len) {
    printf("%s (%zu bytes):\n", title, len);
    for (size_t i = 0; i < len; ++i) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0 || i == len - 1) {
            printf("\n");
        }
    }
    printf("\n");
}

// --- Main Test ---

int main() {
    printf("Starting PacketModifier Bare-Metal Test with SimpleNIC...\n");
    printf("Using nic_send/recv, printf, and tohost for exit.\n");

    // Allocate transmit and receive buffers
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_rx_buf[BUF_SIZE];

    // --- 1. NIC Initialization (Implicit) ---
    printf("SimpleNIC assumed ready after reset.\n");
    uint64_t mac = nic_macaddr();
    printf("NIC MAC Address: %012lx\n", (unsigned long)mac); // Cast for %lx


    // --- 2. Prepare the Test Packet ---
    printf("Preparing test packet (Length: %d bytes)...\n", TEST_PKT_LEN);
    for (int i = 0; i < TEST_PKT_LEN; ++i) {
        tx_buf[i] = (uint8_t)(i + 1); // 0x01, 0x02, ...
    }
    memset(tx_buf + TEST_PKT_LEN, 0, BUF_SIZE - TEST_PKT_LEN);
    memset(rx_buf, 0, BUF_SIZE);

    print_buf("Original TX Data", tx_buf, TEST_PKT_LEN);

    // --- 3. Calculate Expected Received Data ---
    memcpy(expected_rx_buf, tx_buf, TEST_PKT_LEN);
    if (TEST_PKT_LEN > 0) {
        expected_rx_buf[0] = ~tx_buf[0]; // Invert the first byte
    }
    print_buf("Expected RX Data (after modification)", expected_rx_buf, TEST_PKT_LEN);


    // --- 4. Send the Packet ---
    printf("Sending packet using nic_send()...\n");
    nic_send(tx_buf, (unsigned long)TEST_PKT_LEN);
    printf("Packet sent (nic_send completed).\n");


    // --- 5. Receive the Packet (Loopback) ---
    printf("Attempting to receive packet using nic_recv()...\n");
    int received_len = nic_recv(rx_buf);

    // --- 6. Verify the Received Packet ---
    if (received_len <= 0) {
        printf("ERROR: nic_recv returned non-positive length: %d. NIC Error or Timeout?\n", received_len);
        sim_fail(3); // Use distinct failure codes
    }

    printf("Packet received (nic_recv completed, Length: %d bytes).\n", received_len);
    print_buf("Actual RX Data", rx_buf, (size_t)received_len);

    // Check 1: Correct Length?
    if (received_len != TEST_PKT_LEN) {
        printf("ERROR: Received packet length mismatch! Expected %d, Got %d\n",
               TEST_PKT_LEN, received_len);
        sim_fail(4); // Use distinct failure codes
    }

    // Check 2: Correct Data (including modification)?
    if (memcmp(rx_buf, expected_rx_buf, TEST_PKT_LEN) != 0) {
        printf("ERROR: Received packet data mismatch!\n");
        // Find first mismatch for debugging (relies on printf)
        for(int i=0; i < TEST_PKT_LEN; ++i) {
            if (rx_buf[i] != expected_rx_buf[i]) {
                printf("Mismatch at byte %d: Expected 0x%02x, Got 0x%02x\n",
                       i, expected_rx_buf[i], rx_buf[i]);
                break; // Only show first mismatch
            }
        }
        sim_fail(5); // Use distinct failure codes
    }

    // --- 7. Report Success ---
    // Final success message is now inside sim_pass()
    sim_pass(); // Signal success to simulator

    return 0; // Should not be reached
}