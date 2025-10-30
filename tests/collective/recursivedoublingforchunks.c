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
#define NUM_LEVELS (MAX_RECURSION_LEVEL + 1)  // Testing levels 0, 1, 2, 3 (MAX_RECURSION_LEVEL + 1)
#define NUM_ELEMENTS 256      // As per module config
#define BYTES_PER_ELEMENT 4   // As per module config (32-bit elements)
#define METADATA_LEN 16        // Fixed metadata size
#define DATA_PAYLOAD_LEN (NUM_ELEMENTS * BYTES_PER_ELEMENT) // 256 * 4 = 1024
#define TOTAL_PACKET_LEN (METADATA_LEN + DATA_PAYLOAD_LEN)  // 16 + 1024 = 1040

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)
#define NUM_TEST_SETS 128       // Reduced test sets for chunked testing
#define MAX_CHUNKS_PER_LEVEL 8  // Test with up to 4 chunks per level (4KB total)

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

// Generate the nth permutation of numbers 0 to n-1 (0-indexed)
void generate_nth_permutation(int* perm, int n, int nth) {
    // Initialize with sequential numbers
    for (int i = 0; i < n; i++) {
        perm[i] = i;
    }
    
    // Generate the nth permutation using factorial number system
    for (int i = 0; i < n - 1; i++) {
        int fact = 1;
        for (int j = 2; j <= n - 1 - i; j++) {
            fact *= j;
        }
        int pos = nth / fact;
        nth %= fact;
        
        // Move element at pos to position i
        int temp = perm[i + pos];
        for (int j = i + pos; j > i; j--) {
            perm[j] = perm[j - 1];
        }
        perm[i] = temp;
    }
}

// Add these debug print functions after the existing helper functions
void print_packet_metadata(const char* prefix, const uint8_t* buf) {
    printf("%s Metadata (16 bytes):\n", prefix);
    
    // --- Word 0 ---
    printf("  Collective ID: 0x%04x\n", (buf[1] << 8) | buf[0]);
    printf("  Collective Type: 0x%02x\n", buf[2]);
    printf("  Operation: 0x%02x\n", buf[3]);
    printf("  Reserved: 0x%02x%02x\n", buf[4], buf[5]);
    printf("  Max Level: %u\n", buf[6]);
    printf("  Current Level: %u\n", buf[7]);

    // --- Word 1 ---
    uint32_t chunk_index, total_chunks;
    memcpy(&chunk_index, buf + 8, sizeof(uint32_t));
    memcpy(&total_chunks, buf + 12, sizeof(uint32_t));
    printf("  Chunk Index: %u\n", chunk_index);
    printf("  Total Chunks: %u\n", total_chunks);
}

// Helper function to print float elements
void print_elements_f(const char* title, const uint32_t* elements, size_t num_elements) {
    // Cast size_t to unsigned long and use %lu
    printf("%s (%lu elements, %lu bytes total):\n", title, (unsigned long)num_elements, (unsigned long)(num_elements * sizeof(uint32_t)));
    
    // The rest of your function is fine
    for (size_t i = 0; i < num_elements; ++i) {
        float val;
        memcpy(&val, &elements[i], sizeof(float));
        printf("%12.4f ", val); // Print as float
        if ((i + 1) % 8 == 0 || i == num_elements - 1) {
            printf("\n");
        }
    }
    printf("\n");
}

// --- Main Test ---

int main() {
    printf("Starting RecursiveDoubling Bare-Metal Test with SimpleNIC...\n");
    srand(1234);
    fflush(stdout);
    printf("Running %d test sets with all possible packet orderings\n", NUM_TEST_SETS);
    printf("Each set: %d levels, %d elements (%d bytes payload per chunk)\n",
           NUM_LEVELS, NUM_ELEMENTS, DATA_PAYLOAD_LEN);
    printf("Max Recursion Level: %d\n", MAX_RECURSION_LEVEL);
    #if DEBUG_PRINT_PACKETS
        printf(">>> Full packet debug printing is ENABLED <<<\n");
    #else
        printf(">>> Full packet debug printing is DISABLED <<<\n");
    #endif

    // No random seed needed for systematic testing

    // Allocate buffers (static for bare-metal)
    // Ensure alignment for potential DMA requirements by NIC
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_rx_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Buffers to hold the chunked input data payloads (as 32-bit elements)
    // Use float for easier calculation, then cast to uint32_t for transmission
    static float input_elements_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static float expected_outputs_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t input_elements[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t expected_outputs[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static int packet_order[NUM_LEVELS * MAX_CHUNKS_PER_LEVEL];  // Order for all chunk packets

    // --- 1. NIC Initialization (Implicit) ---
    printf("SimpleNIC assumed ready after reset.\n");
    uint64_t mac = nic_macaddr();
    printf("NIC MAC Address: %012lx\n", (unsigned long)mac);

    // --- 2. Run Multiple Test Sets ---
    for (int test_set = 0; test_set < NUM_TEST_SETS; test_set++) {
        printf("\n=== Starting Test Set %d ===\n", test_set + 1);

        // Generate unique collective ID for this test set
        uint16_t test_collective_id = (uint16_t)(0x1000 + test_set); // Sequential ID starting from 0x1000
        printf("Collective ID for this set: 0x%04X\n", test_collective_id);

        // For chunked testing, we'll test with a specific number of chunks
        uint32_t total_chunks = MAX_CHUNKS_PER_LEVEL;
        printf("Testing with %u chunks per level\n", total_chunks);
        
        // Create array of all packets to be sent and randomize the send order
        int total_packets = NUM_LEVELS * total_chunks;
        for (int i = 0; i < total_packets; i++) {
            packet_order[i] = i;
        }
        
        // Use permutation function to shuffle the send order for this test set
        // Use test_set as seed for the permutation to get different orderings per test
        generate_nth_permutation(packet_order, total_packets, test_set);
        
        printf("Total packets to send: %d (randomized order)\n", total_packets);
        printf("Send order for test set %d: ", test_set);
        for (int i = 0; i < total_packets; i++) {
            printf("%d ", packet_order[i]);
        }
        printf("\n");

        // Prepare chunked input data for this test set
        float max_rand_val = 1000.0f; // Set your desired maximum random value
        for (int p = 0; p < NUM_LEVELS; ++p) {
            for (int chunk = 0; chunk < total_chunks; ++chunk) {
                for (int i = 0; i < NUM_ELEMENTS; ++i) {
                    // Generate a random float between 0.0 and max_rand_val with varied decimals
                    input_elements_f[p][chunk][i] = ((float)rand() / (float)RAND_MAX) * max_rand_val;
                    // Copy the bit pattern into the uint32_t array for memcpy
                    memcpy(&input_elements[p][chunk][i], &input_elements_f[p][chunk][i], sizeof(uint32_t));
                }
            }
        }

        // Pre-calculate and store all expected outputs for each level and chunk using floating-point math
        for (int chunk = 0; chunk < total_chunks; ++chunk) {
            // Level 0: output equals input
            memcpy(expected_outputs_f[0][chunk], input_elements_f[0][chunk], DATA_PAYLOAD_LEN);
            
            // Higher levels: output = input + previous_level_output
            for (int level = 1; level <= MAX_RECURSION_LEVEL; level++) {
                for (int i = 0; i < NUM_ELEMENTS; i++) {
                    expected_outputs_f[level][chunk][i] = input_elements_f[level][chunk][i] + expected_outputs_f[level-1][chunk][i];
                }
            }
        }

        // After calculating all expected float values, copy their bit patterns to the uint32_t array for verification
        for (int p = 0; p < NUM_LEVELS; p++) {
            for (int chunk = 0; chunk < total_chunks; ++chunk) {
                memcpy(expected_outputs[p][chunk], expected_outputs_f[p][chunk], DATA_PAYLOAD_LEN);
            }
        }

        // --- Non-blocking send/receive to avoid deadlock ---
        printf("\n--- Sending packets and polling for responses ---\n");
        int total_packets_to_send = NUM_LEVELS * total_chunks;
        int total_expected_responses = NUM_LEVELS * total_chunks;
        int responses_received = 0;
        int packets_sent = 0;
        int received_chunks[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL]; // Track which chunks we've received

        // A stall detector
        const uint64_t STALL_TIMEOUT_CYCLES = 10000000; // Adjust as needed
        uint64_t stall_counter = 0;

        // Initialize tracking array
        for (int l = 0; l < NUM_LEVELS; l++) {
            for (int c = 0; c < MAX_CHUNKS_PER_LEVEL; c++) {
                received_chunks[l][c] = 0;
            }
        }

        // --- Arm the NIC by pre-posting a receive buffer ---
        printf("Pre-posting initial receive buffer.\n");
        while (nic_recv_req_avail() == 0); // Wait until NIC can accept a receive request
        reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);

        // --- Main polling loop ---
        while (responses_received < total_expected_responses) {
            
            // === 1. POLL AND TRY TO SEND ===
            // Can we send? (NIC has buffer space AND we have packets left to send)
            if (nic_send_req_avail() > 0 && packets_sent < total_packets_to_send) {
                int packet_index = packet_order[packets_sent];
                int level = packet_index / total_chunks;
                int chunk = packet_index % total_chunks;

                #if DEBUG_PRINT_PACKETS
                    printf("Sending Packet %d/%d (order[%d]=%d): Level %d, Chunk %d\n", packets_sent+1, total_packets_to_send, packets_sent, packet_index, level, chunk);
                #endif

                // Construct TX Packet - Word 0
                memset(tx_buf, 0, BUF_SIZE);
                tx_buf[0] = (uint8_t)(test_collective_id & 0xFF);
                tx_buf[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                tx_buf[2] = META_COLL_TYPE;
                tx_buf[3] = META_OP;
                tx_buf[4] = 0x00;
                tx_buf[5] = 0x00;
                tx_buf[6] = MAX_RECURSION_LEVEL;
                tx_buf[7] = (uint8_t)level;

                // --- Word 1 (Offset 8): Chunked Metadata ---
                uint32_t chunk_index = (uint32_t)chunk;
                memcpy(tx_buf + 8, &chunk_index, sizeof(uint32_t));
                memcpy(tx_buf + 12, &total_chunks, sizeof(uint32_t));

                // Copy chunk data
                memcpy(tx_buf + METADATA_LEN, input_elements[level][chunk], DATA_PAYLOAD_LEN);

                nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Sent Packet Details ---\n");
                    print_packet_metadata("TX", tx_buf);
                    print_elements_f("TX", (const uint32_t*)(tx_buf + METADATA_LEN), 8);
                #endif

                packets_sent++;
                stall_counter = 0; // Reset stall counter because we made progress
            }

            // === 2. POLL FOR A COMPLETED RECEIVE ===
            // Has the NIC filled our pre-posted buffer?
            if (nic_recv_comp_avail() > 0) {
                // Acknowledge the completion and get the packet length
                int received_len = reg_read16(SIMPLENIC_RECV_COMP);
                asm volatile ("fence");

                // Verify that the packet length is what we expect.
                // Any other length indicates a critical error in the DUT or NIC.
                if (received_len != TOTAL_PACKET_LEN) {
                    printf("ERROR: Received packet with unexpected length! Expected %d, Got %d\n",
                           TOTAL_PACKET_LEN, received_len);
                    sim_fail(600 + test_set);
                }

                // --- Now, process the packet we just received ---
                // (The verification logic is identical to your original code)
                uint8_t response_level = rx_buf[7];
                uint32_t response_chunk_index, response_total_chunks;
                memcpy(&response_chunk_index, rx_buf + 8, sizeof(uint32_t));
                memcpy(&response_total_chunks, rx_buf + 12, sizeof(uint32_t));

                #if DEBUG_PRINT_PACKETS
                    printf("Received response for level %u, chunk %u\n", response_level, response_chunk_index);
                #endif
                
                if (received_chunks[response_level-1][response_chunk_index]) {
                    printf("ERROR: Duplicate response for level %u, chunk %u\n", response_level, response_chunk_index);
                    sim_fail(300 + test_set);
                }
                received_chunks[response_level-1][response_chunk_index] = 1;

                // Construct expected response using the pre-calculated expected output
                memset(expected_rx_buf, 0, BUF_SIZE);
                // Construct metadata for expected response
                expected_rx_buf[0] = (uint8_t)(test_collective_id & 0xFF);
                expected_rx_buf[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                expected_rx_buf[2] = META_COLL_TYPE;
                expected_rx_buf[3] = META_OP;
                expected_rx_buf[4] = 0x00;
                expected_rx_buf[5] = 0x00;
                expected_rx_buf[6] = MAX_RECURSION_LEVEL;
                expected_rx_buf[7] = response_level;

                // --- Word 1 (Offset 8): Expected Chunked Metadata ---
                memcpy(expected_rx_buf + 8, &response_chunk_index, sizeof(uint32_t));
                memcpy(expected_rx_buf + 12, &response_total_chunks, sizeof(uint32_t));

                // Copy expected chunk data
                memcpy(expected_rx_buf + METADATA_LEN, expected_outputs[response_level-1][response_chunk_index], DATA_PAYLOAD_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Received Packet Details ---\n");
                    print_packet_metadata("RX", rx_buf);
                    print_elements_f("RX", (const uint32_t*)(rx_buf + METADATA_LEN), 8);

                    printf("\n--- Expected Packet Details ---\n");
                    print_packet_metadata("Expected", expected_rx_buf);
                    print_elements_f("Expected", (const uint32_t*)(expected_rx_buf + METADATA_LEN), 8);
                #endif

                // Verify response
                // 1) Strictly compare metadata
                if (memcmp(rx_buf, expected_rx_buf, METADATA_LEN) != 0) {
                    printf("ERROR: Metadata mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                    sim_fail(400 + test_set);
                }

                // 2) Compare payload with 1-ULP tolerance per 32-bit float element
                {
                    const uint32_t* exp_words = (const uint32_t*)(expected_rx_buf + METADATA_LEN);
                    const uint32_t* got_words = (const uint32_t*)(rx_buf + METADATA_LEN);
                    int bad_elem = -1;
                    uint32_t bad_exp = 0, bad_got = 0, bad_diff = 0;

                    for (int e = 0; e < NUM_ELEMENTS; ++e) {
                        uint32_t a = exp_words[e];
                        uint32_t b = got_words[e];
                        if (a == b) continue;

                        int32_t ai, bi;
                        memcpy(&ai, &a, sizeof(int32_t));
                        memcpy(&bi, &b, sizeof(int32_t));
                        if (ai < 0) ai = 0x80000000 - ai;
                        if (bi < 0) bi = 0x80000000 - bi;
                        uint32_t udiff = (ai > bi) ? (uint32_t)(ai - bi) : (uint32_t)(bi - ai);

                        if (udiff > 1u) {
                            bad_elem = e;
                            bad_exp = a;
                            bad_got = b;
                            bad_diff = udiff;
                            break;
                        }
                    }

                    if (bad_elem >= 0) {
                        printf("ERROR: Float payload mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                        printf("  First differing element %d: Expected 0x%08x Got 0x%08x (ULP diff %u)\n",
                               bad_elem, bad_exp, bad_got, bad_diff);
                        sim_fail(400 + test_set);
                    }
                }

                #if DEBUG_PRINT_PACKETS
                    printf("Response for level %u, chunk %u verified successfully.\n", response_level, response_chunk_index);
                #endif
                
                responses_received++;
                stall_counter = 0; // Reset stall counter because we made progress

                // --- If more packets are expected, re-post the buffer ---
                if (responses_received < total_expected_responses) {
                    while (nic_recv_req_avail() == 0);
                    reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
                }
            }

            // === 3. CHECK FOR STALL ===
            if (packets_sent == total_packets_to_send && responses_received < total_expected_responses) {
                stall_counter++;
                if (stall_counter > STALL_TIMEOUT_CYCLES) {
                    printf("ERROR: Stall detected! Sent all packets but timed out waiting for responses.\n");
                    printf("Sent: %d, Received: %d\n", packets_sent, responses_received);
                    sim_fail(500 + test_set);
                }
            }
        } // End of main polling loop

        printf("All %d packets sent and %d expected responses received.\n", packets_sent, responses_received);
        printf("=== Test Set %d Completed Successfully ===\n", test_set + 1);
    }

    printf("\n--- All %d Test Sets Completed Successfully ---\n", NUM_TEST_SETS);
    sim_pass();
    return 0;
}

