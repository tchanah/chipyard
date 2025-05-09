#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // For size_t, NULL etc.
#include <time.h>

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
#define NUM_TEST_SETS 5  // Number of different test sets to run

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

// Generate a random permutation of numbers 0 to n-1
void generate_random_permutation(int* perm, int n) {
    // Initialize with sequential numbers
    for (int i = 0; i < n; i++) {
        perm[i] = i;
    }
    
    // Fisher-Yates shuffle
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        printf("Shuffle: i=%d, j=%d\n", i, j);
        // Swap
        int temp = perm[i];
        perm[i] = perm[j];
        perm[j] = temp;
    }
}

// Calculate expected output for a given level and input data
void calculate_expected_output(uint32_t* expected, const uint32_t* input, 
                            const uint32_t* previous_output, int level) {
    if (level == 0) {
        // Level 0: Output is just the input
        memcpy(expected, input, DATA_PAYLOAD_LEN);
    } else {
        // Level > 0: Output is input + previous level's output
        for (int i = 0; i < NUM_ELEMENTS; ++i) {
            expected[i] = input[i] + previous_output[i];
        }
    }
}

// Add these debug print functions after the existing helper functions
void print_packet_metadata(const char* prefix, const uint8_t* buf) {
    printf("%s Metadata:\n", prefix);
    printf("  Collective ID: 0x%04x\n", (buf[1] << 8) | buf[0]);
    printf("  Collective Type: 0x%02x\n", buf[2]);
    printf("  Operation: 0x%02x\n", buf[3]);
    printf("  Reserved: 0x%02x%02x\n", buf[4], buf[5]);
    printf("  Max Level: %u\n", buf[6]);
    printf("  Current Level: %u\n", buf[7]);
}

void print_packet_data(const char* prefix, const uint8_t* buf) {
    printf("%s First 8 Data Elements:\n", prefix);
    const uint32_t* elements = (const uint32_t*)(buf + METADATA_LEN);
    for (int i = 0; i < 8; i++) {
        printf("  Element[%d]: 0x%08x\n", i, elements[i]);
    }
    printf("\n");
}

// --- Main Test ---

int main() {
    printf("Starting RecursiveDoubling Bare-Metal Test with SimpleNIC...\n");
    printf("Running %d test sets with random packet orderings\n", NUM_TEST_SETS);
    printf("Each set: %d packets, %d elements (%d bytes payload)\n",
           NUM_PACKETS, NUM_ELEMENTS, DATA_PAYLOAD_LEN);
    printf("Max Recursion Level: %d\n", MAX_RECURSION_LEVEL);
    #if DEBUG_PRINT_PACKETS
        printf(">>> Full packet debug printing is ENABLED <<<\n");
    #else
        printf(">>> Full packet debug printing is DISABLED <<<\n");
    #endif

    // Initialize random seed with a better method
    unsigned int seed = (unsigned int)time(NULL);
    if (seed == (unsigned int)-1) {
        // If time() fails, use a fallback method
        seed = (unsigned int)((uintptr_t)&seed);  // Use address as fallback
    }
    srand(seed);
    printf("Random seed: %u\n", seed);

    // Allocate buffers (static for bare-metal)
    // Ensure alignment for potential DMA requirements by NIC
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_rx_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Buffers to hold the input data payloads (as 32-bit elements)
    static uint32_t input_elements[NUM_PACKETS][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t expected_outputs[NUM_PACKETS][NUM_ELEMENTS] __attribute__((aligned(64)));
    static int packet_order[NUM_PACKETS];

    // --- 1. NIC Initialization (Implicit) ---
    printf("SimpleNIC assumed ready after reset.\n");
    uint64_t mac = nic_macaddr();
    printf("NIC MAC Address: %012lx\n", (unsigned long)mac);

    // --- 2. Run Multiple Test Sets ---
    for (int test_set = 0; test_set < NUM_TEST_SETS; test_set++) {
        printf("\n=== Starting Test Set %d ===\n", test_set + 1);

        // Generate random packet order for this test set
        generate_random_permutation(packet_order, NUM_PACKETS);
        printf("Packet order for this set: ");
        for (int i = 0; i < NUM_PACKETS; i++) {
            printf("%d ", packet_order[i]);
        }
        printf("\n");

        // Prepare input data for this test set
        for (int p = 0; p < NUM_PACKETS; ++p) {
            for (int i = 0; i < NUM_ELEMENTS; ++i) {
                input_elements[p][i] = (uint32_t)((test_set * 10000) + (p * 1000) + i + 1);
            }
        }

        // Pre-calculate and store all expected outputs for each level
        // Level 0: Just store input data
        memcpy(expected_outputs[0], input_elements[0], DATA_PAYLOAD_LEN);
        
        // Level 1 to MAX_RECURSION_LEVEL: Add previous level's output
        for (int level = 1; level <= MAX_RECURSION_LEVEL; level++) {
            for (int i = 0; i < NUM_ELEMENTS; i++) {
                expected_outputs[level][i] = input_elements[level][i] + expected_outputs[level-1][i];
            }
        }

        // --- Phase 1: Send all packets in random order ---
        printf("\n--- Sending all packets ---\n");
        for (int i = 0; i < NUM_PACKETS; ++i) {
            int p = packet_order[i];
            uint8_t current_input_level = (uint8_t)p;

            printf("Sending Packet %d (Level %u) in position %d...\n", 
                   p, current_input_level, i);

            // Construct TX Packet
            memset(tx_buf, 0, BUF_SIZE);
            tx_buf[0] = (uint8_t)(META_COLL_ID & 0xFF);
            tx_buf[1] = (uint8_t)((META_COLL_ID >> 8) & 0xFF);
            tx_buf[2] = META_COLL_TYPE;
            tx_buf[3] = META_OP;
            tx_buf[4] = 0x00;
            tx_buf[5] = 0x00;
            tx_buf[6] = MAX_RECURSION_LEVEL;
            tx_buf[7] = current_input_level;

            memcpy(tx_buf + METADATA_LEN, input_elements[p], DATA_PAYLOAD_LEN);

            // Send packet
            nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);

            #if DEBUG_PRINT_PACKETS
                printf("\n--- Sent Packet Details ---\n");
                print_packet_metadata("TX", tx_buf);
                print_packet_data("TX", tx_buf);
            #endif
        }
        printf("All packets sent.\n");

        // --- Phase 2: Receive and verify all responses ---
        printf("\n--- Receiving and verifying responses ---\n");
        int responses_received = 0;
        int received_levels[NUM_PACKETS] = {0}; // Track which levels we've received

        while (responses_received < NUM_PACKETS) {
            // Receive a response
            memset(rx_buf, 0, BUF_SIZE);
            int received_len = nic_recv(rx_buf);

            if (received_len <= 0) {
                printf("ERROR: nic_recv returned non-positive length: %d\n", received_len);
                sim_fail(100 + test_set);
            }

            if (received_len != TOTAL_PACKET_LEN) {
                printf("ERROR: Received packet length mismatch! Expected %d, Got %d\n",
                       TOTAL_PACKET_LEN, received_len);
                sim_fail(200 + test_set);
            }

            // Extract level from response metadata
            uint8_t response_level = rx_buf[7];
            printf("Received response for level %u\n", response_level);

            // Verify this level hasn't been received before
            if (received_levels[response_level-1]) {
                printf("ERROR: Duplicate response received for level %u\n", response_level);
                sim_fail(300 + test_set);
            }
            received_levels[response_level-1] = 1;

            // Construct expected response using the pre-calculated expected output
            memset(expected_rx_buf, 0, BUF_SIZE);
            // Construct metadata for expected response
            expected_rx_buf[0] = (uint8_t)(META_COLL_ID & 0xFF);
            expected_rx_buf[1] = (uint8_t)((META_COLL_ID >> 8) & 0xFF);
            expected_rx_buf[2] = META_COLL_TYPE;
            expected_rx_buf[3] = META_OP;
            expected_rx_buf[4] = 0x00;
            expected_rx_buf[5] = 0x00;
            expected_rx_buf[6] = MAX_RECURSION_LEVEL;
            expected_rx_buf[7] = response_level;
            
            // Use the pre-calculated expected output for this level
            memcpy(expected_rx_buf + METADATA_LEN, expected_outputs[response_level-1], DATA_PAYLOAD_LEN);

            #if DEBUG_PRINT_PACKETS
                printf("\n--- Received Packet Details ---\n");
                print_packet_metadata("RX", rx_buf);
                print_packet_data("RX", rx_buf);
                
                printf("\n--- Expected Packet Details ---\n");
                print_packet_metadata("Expected", expected_rx_buf);
                print_packet_data("Expected", expected_rx_buf);
            #endif

            // Verify response
            if (memcmp(rx_buf, expected_rx_buf, TOTAL_PACKET_LEN) != 0) {
                printf("ERROR: Response data mismatch for level %u!\n", response_level);
                for(int i=0; i < TOTAL_PACKET_LEN; ++i) {
                    if (rx_buf[i] != expected_rx_buf[i]) {
                        printf("  Mismatch at byte %d: Expected 0x%02x, Got 0x%02x\n",
                               i, expected_rx_buf[i], rx_buf[i]);
                        if (i >= METADATA_LEN) {
                            int element_idx = (i - METADATA_LEN) / BYTES_PER_ELEMENT;
                            printf("    (Element %d)\n", element_idx);
                        } else {
                            printf("    (Metadata byte)\n");
                        }
                        break;
                    }
                }
                sim_fail(400 + test_set);
            }

            printf("Response for level %u verified successfully.\n", response_level);
            responses_received++;
        }

        printf("=== Test Set %d Completed Successfully ===\n", test_set + 1);
    }

    printf("\n--- All %d Test Sets Completed Successfully ---\n", NUM_TEST_SETS);
    sim_pass();
    return 0;
}