#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // For size_t, NULL etc.

// --- MMIO and NIC Driver Headers ---
#include "../mmio.h" // Assumed to be in ../ relative to the C file
#include "../nic.h"  // Assumed to be in ../ relative to the C file

// --- Simulation Control via 'tohost' ---
extern volatile uint64_t tohost;

// Functions to signal simulation pass/fail
static inline void sim_pass() {
    printf("SUCCESS: Test PASSED. Signaling simulation success.\n");
    // fflush(stdout); // Optional: may not work/be needed in bare-metal
    tohost = 1; // Standard encoding for success
    while (1);
}

static inline void sim_fail(uint64_t code) {
    printf("ERROR: Test FAILED with code %lu. Signaling simulation failure.\n", (unsigned long)code);
    // fflush(stdout); // Optional
    if (code == 0) code = 0xFF; // Ensure failure code is non-zero
    tohost = (code << 1) | 1; // Standard encoding for failure
    while (1);
}

// --- Test Configuration ---
#define BUF_SIZE 2048         // Should be >= TOTAL_PACKET_LEN
#define NUM_PACKETS 4         // Testing levels 0, 1, 2, 3
#define NUM_ELEMENTS 256      // As per module config
#define BYTES_PER_ELEMENT 4   // As per module config (32-bit elements)
#define METADATA_LEN 8        // Fixed metadata size
#define DATA_PAYLOAD_LEN (NUM_ELEMENTS * BYTES_PER_ELEMENT) // 256 * 4 = 1024
#define TOTAL_PACKET_LEN (METADATA_LEN + DATA_PAYLOAD_LEN)  // 8 + 1024 = 1032

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)

// Define metadata values (example)
#define META_COLL_ID   0xABCD
#define META_COLL_TYPE 0x01
#define META_OP        0x05 // e.g., 5 means ADD

#define DEBUG_PRINT_PACKETS 0 // Set to 1 to print full TX/RX packets, 0 to disable

// Check buffer size at compile time (optional)
#if BUF_SIZE < TOTAL_PACKET_LEN
#error "BUF_SIZE is too small!"
#endif

// --- Helper Functions ---

// Prints a buffer in hex
void print_buf_hex(const char* title, const uint8_t* buf, size_t len) {
    printf("%s (%zu bytes):\n", title, len);
    for (size_t i = 0; i < len; ++i) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        } else if ((i + 1) % 8 == 0) {
            printf(" "); // Add space after 8 bytes
        }
    }
    if (len % 16 != 0) { // Print trailing newline if needed
        printf("\n");
    }
    printf("\n");
}

// Prints data payload as 32-bit elements (hex)
void print_elements(const char* title, const uint32_t* elements, size_t num_elements) {
    printf("%s (%zu elements, %zu bytes total):\n", title, num_elements, num_elements * sizeof(uint32_t));
    for (size_t i = 0; i < num_elements; ++i) {
        printf("0x%08x ", elements[i]);
        if ((i + 1) % 8 == 0 || i == num_elements - 1) {
            printf("\n");
        }
    }
    printf("\n");
}


// --- Main Test ---

int main() {
    printf("Starting RecursiveDoubling Bare-Metal Test with SimpleNIC...\n");
    printf("Sending %d packets, %d elements (%d bytes payload) each.\n",
           NUM_PACKETS, NUM_ELEMENTS, DATA_PAYLOAD_LEN);
    printf("Max Recursion Level set in packets: %d\n", MAX_RECURSION_LEVEL);
    #if DEBUG_PRINT_PACKETS
        printf(">>> Full packet debug printing is ENABLED <<<\n");
    #else
        printf(">>> Full packet debug printing is DISABLED <<<\n");
    #endif

    // Allocate buffers (static for bare-metal)
    // Ensure alignment for potential DMA requirements by NIC
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_rx_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Buffers to hold the input data payloads (as 32-bit elements)
    static uint32_t input_elements[NUM_PACKETS][NUM_ELEMENTS] __attribute__((aligned(64)));

    // Buffer to hold the *expected* data payload result after processing
    // This will store the running sum needed for subsequent calculations
    static uint32_t expected_payload_elements[NUM_ELEMENTS] __attribute__((aligned(64)));
    // Buffer to hold the expected payload from the *previous* step (needed for calculation)
    static uint32_t previous_expected_payload_elements[NUM_ELEMENTS] __attribute__((aligned(64)));


    // --- 1. NIC Initialization (Implicit) ---
    printf("SimpleNIC assumed ready after reset.\n");
    uint64_t mac = nic_macaddr();
    printf("NIC MAC Address: %012lx\n", (unsigned long)mac);


    // --- 2. Prepare Input Data Payloads ---
    printf("Preparing distinct input data for %d packets...\n", NUM_PACKETS);
    for (int p = 0; p < NUM_PACKETS; ++p) {
        for (int i = 0; i < NUM_ELEMENTS; ++i) {
            // Simple pattern: packet_index * 1000 + element_index + 1
            input_elements[p][i] = (uint32_t)((p * 1000) + i + 1);
        }
        // Optional: Print the first few elements of each input packet
        // print_elements("Input Data Pkt", input_elements[p], 8); // Print first 8
    }
    printf("Input data preparation complete.\n");


    // --- 3. Send/Receive/Verify Loop ---
    printf("\n--- Starting Packet Send/Receive Loop ---\n");
    for (int p = 0; p < NUM_PACKETS; ++p) {
        uint8_t current_input_level = (uint8_t)p; // Packet 0 has level 0, etc.
        uint8_t expected_output_level = current_input_level + 1;

        printf("\nProcessing Packet %d (Input Level: %u)...\n", p, current_input_level);

        // --- 3a. Construct TX Packet ---
        memset(tx_buf, 0, BUF_SIZE); // Clear buffer first

        // Set Metadata (Bytes 0-7)
        tx_buf[0] = (uint8_t)(META_COLL_ID & 0xFF);        // Coll ID LSB
        tx_buf[1] = (uint8_t)((META_COLL_ID >> 8) & 0xFF); // Coll ID MSB
        tx_buf[2] = META_COLL_TYPE;                        // Type
        tx_buf[3] = META_OP;                               // Operation
        tx_buf[4] = 0x00;                                  // Empty
        tx_buf[5] = 0x00;                                  // Empty
        tx_buf[6] = MAX_RECURSION_LEVEL;                   // Max Level
        tx_buf[7] = current_input_level;                   // Current Level

        // Copy Data Payload (elements start after metadata)
        memcpy(tx_buf + METADATA_LEN, input_elements[p], DATA_PAYLOAD_LEN);

        // Optional: Print TX buffer
        #if DEBUG_PRINT_PACKETS
            print_buf_hex("TX Buf (Packet)", tx_buf, TOTAL_PACKET_LEN);
            //print_buf_hex("TX Buf (Metadata)", tx_buf, METADATA_LEN);
            //print_elements("TX Payload Elements", (uint32_t*)(tx_buf + METADATA_LEN), NUM_ELEMENTS);
        #endif

        // --- 3b. Calculate Expected RX Packet ---
        memset(expected_rx_buf, 0, BUF_SIZE);

        // Calculate Expected Metadata
        expected_rx_buf[0] = tx_buf[0]; // Copy most metadata from TX
        expected_rx_buf[1] = tx_buf[1];
        expected_rx_buf[2] = tx_buf[2];
        expected_rx_buf[3] = tx_buf[3];
        expected_rx_buf[4] = 0x00;
        expected_rx_buf[5] = 0x00;
        expected_rx_buf[6] = MAX_RECURSION_LEVEL;
        expected_rx_buf[7] = expected_output_level; // *** Output Level is incremented ***

        // Calculate Expected Data Payload
        printf("Calculating expected output payload (Input Level %u)...\n", current_input_level);
        if (current_input_level == 0) {
            // Level 0: Output payload is just the input payload
            memcpy(expected_payload_elements, input_elements[0], DATA_PAYLOAD_LEN);
            printf("  Level 0: Expected payload = Input payload of Pkt 0.\n");
        } else {
            // Level > 0: Output is input[p] + previous_expected_payload
            // 'previous_expected_payload_elements' holds the expected output from the *previous* packet (which corresponds to storage[p-1])
            printf("  Level >0: Expected payload = Input payload Pkt %d + Prev Expected Payload (from Pkt %d output).\n", p, p-1);
            for (int i = 0; i < NUM_ELEMENTS; ++i) {
                // Perform element-wise addition (watch for overflow if not intended)
                expected_payload_elements[i] = input_elements[p][i] + previous_expected_payload_elements[i];
            }
        }

        // Copy calculated payload elements into the expected RX buffer
        memcpy(expected_rx_buf + METADATA_LEN, expected_payload_elements, DATA_PAYLOAD_LEN);

        // Optional: Print Expected RX Buffer
        #if DEBUG_PRINT_PACKETS
            print_buf_hex("Expected RX Buf (Packet)", expected_rx_buf, TOTAL_PACKET_LEN);
            //print_buf_hex("Expected RX Buf (Metadata)", expected_rx_buf, METADATA_LEN);
            //print_elements("Expected RX Payload Elements", (uint32_t*)(expected_rx_buf + METADATA_LEN), NUM_ELEMENTS);
        #endif


        // --- 3c. Send the Packet ---
        printf("Sending packet %d using nic_send()...\n", p);
        nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);
        printf("Packet %d sent.\n", p);


        // --- 3d. Receive the Packet ---
        printf("Attempting to receive packet %d using nic_recv()...\n", p);
        memset(rx_buf, 0, BUF_SIZE); // Clear RX buffer before receive
        int received_len = nic_recv(rx_buf);


        // --- 3e. Verify Received Packet ---
        if (received_len <= 0) {
            printf("ERROR (Packet %d): nic_recv returned non-positive length: %d.\n", p, received_len);
            sim_fail(10 + p); // Failure code based on packet index
        }

        printf("Packet %d received (Length: %d bytes).\n", p, received_len);
        // Optional: Print actual received buffer
        #if DEBUG_PRINT_PACKETS
            print_buf_hex("Actual RX Buf (Packet)", rx_buf, (size_t)received_len);
            //print_buf_hex("Actual RX Buf (Metadata)", rx_buf, METADATA_LEN);
            //print_elements("Actual RX Payload Elements", (uint32_t*)(rx_buf + METADATA_LEN), NUM_ELEMENTS);
        #endif


        // Check 1: Correct Length?
        if (received_len != TOTAL_PACKET_LEN) {
            printf("ERROR (Packet %d): Received packet length mismatch! Expected %d, Got %d\n",
                   p, TOTAL_PACKET_LEN, received_len);
            sim_fail(20 + p);
        } else {
             printf("  Length check OK.\n");
        }

        // Check 2: Correct Data (Metadata + Payload)?
        if (memcmp(rx_buf, expected_rx_buf, TOTAL_PACKET_LEN) != 0) {
            printf("ERROR (Packet %d): Received packet data mismatch!\n", p);
            // Find first mismatch for debugging
            for(int i=0; i < TOTAL_PACKET_LEN; ++i) {
                if (rx_buf[i] != expected_rx_buf[i]) {
                    printf("  Mismatch at byte %d: Expected 0x%02x, Got 0x%02x\n",
                           i, expected_rx_buf[i], rx_buf[i]);
                    if (i >= METADATA_LEN) {
                        int element_idx = (i - METADATA_LEN) / BYTES_PER_ELEMENT;
                        int byte_in_elem = (i - METADATA_LEN) % BYTES_PER_ELEMENT;
                        printf("    (Element %d, Byte %d)\n", element_idx, byte_in_elem);
                        // Also print the full expected/actual elements
                        uint32_t* rx_elem_ptr = (uint32_t*)(rx_buf + METADATA_LEN);
                        uint32_t* exp_elem_ptr = (uint32_t*)(expected_rx_buf + METADATA_LEN);
                        printf("    Expected Element: 0x%08x, Got Element: 0x%08x\n", exp_elem_ptr[element_idx], rx_elem_ptr[element_idx]);

                    } else {
                        printf("    (Metadata byte)\n");
                    }
                    break; // Only show first mismatch
                }
            }
            sim_fail(30 + p);
        } else {
             printf("  Data content check OK.\n");
        }

        // --- 3f. Prepare for Next Iteration ---
        // Copy the payload we just calculated/verified into the 'previous' buffer
        // This becomes the input for the *next* iteration's addition step.
        memcpy(previous_expected_payload_elements, expected_payload_elements, DATA_PAYLOAD_LEN);

    } // End of packet loop

    // --- 4. Report Success ---
    printf("\n--- All %d Packets Processed and Verified Successfully ---\n", NUM_PACKETS);
    sim_pass(); // Signal success to simulator

    return 0; // Should not be reached
}