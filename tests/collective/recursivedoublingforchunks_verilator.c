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
#define ETH_HEADER_LEN 16      // Ethernet header (14 bytes + 2 bytes padding)
#define METADATA_LEN 16        // Fixed metadata size
#define DATA_PAYLOAD_LEN (NUM_ELEMENTS * BYTES_PER_ELEMENT) // 256 * 4 = 1024
#define TOTAL_PACKET_LEN (ETH_HEADER_LEN + METADATA_LEN + DATA_PAYLOAD_LEN)  // 16 + 16 + 1024 = 1056

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)
#define NUM_TEST_SETS 1       // Reduced test sets for chunked testing
#define MAX_CHUNKS_PER_LEVEL 4  // Test with up to 4 chunks per level (4KB total)

// Define metadata values (example)
#define META_COLL_ID   0xABCD
#define META_COLL_TYPE 0x01
#define META_OP        0x05 // e.g., 5 means ADD

#define DEBUG_PRINT_PACKETS 1 // Set to 1 to print full TX/RX packets, 0 to disable
#define VERIFY_MAC_ROUTING 1   // Set to 1 to verify MAC routing logic

// MAC address constants (matching hardware)
#define BASE_MAC 0x00126D000000ULL  // Base MAC: 00:12:6D:00:00:00
#define TEST_NODE_RANK 0            // Node rank for this test (0-7 for 8 nodes)
#define TEST_PYTORCH_MAC 0x00126D0000FFULL  // Simulated PyTorch source MAC

// Check buffer size at compile time (optional)
#if BUF_SIZE < TOTAL_PACKET_LEN
#error "BUF_SIZE is too small!"
#endif

// --- Helper Functions ---

// Calculate partner rank for recursive doubling: partner = myRank XOR (1 << level)
// This matches the hardware logic in RecursiveDoublingWithDMA.scala
static inline uint8_t calculate_partner_rank(uint8_t level, uint8_t my_rank) {
    uint8_t distance = 1U << level;
    return my_rank ^ distance;
}

// Calculate partner MAC address: baseMac | partnerRank
// This matches the hardware logic in RecursiveDoublingWithDMA.scala
static inline uint64_t calculate_partner_mac(uint8_t level, uint8_t my_rank) {
    uint8_t partner_rank = calculate_partner_rank(level, my_rank);
    return BASE_MAC | (uint64_t)partner_rank;
}

// Print MAC address in readable format
static void print_mac(const char* label, uint64_t mac) {
    printf("%s: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx (0x%012lx)\n",
           label,
           (unsigned long)((mac >> 40) & 0xFF), (unsigned long)((mac >> 32) & 0xFF), 
           (unsigned long)((mac >> 24) & 0xFF), (unsigned long)((mac >> 16) & 0xFF), 
           (unsigned long)((mac >> 8) & 0xFF), (unsigned long)(mac & 0xFF),
           (unsigned long)mac);
}

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
    
    #if VERIFY_MAC_ROUTING
    printf("\n--- MAC Routing Verification Setup ---\n");
    printf("Test Node Rank: %d (configured via +node_rank PlusArg)\n", TEST_NODE_RANK);
    print_mac("Base MAC", BASE_MAC);
    print_mac("Test Node MAC", BASE_MAC | TEST_NODE_RANK);
    print_mac("Simulated PyTorch Source MAC", TEST_PYTORCH_MAC);
    printf("\nExpected Partner MACs for each level:\n");
    for (uint8_t level = 1; level <= MAX_RECURSION_LEVEL; level++) {
        uint64_t partner_mac = calculate_partner_mac(level, TEST_NODE_RANK);
        uint8_t partner_rank = calculate_partner_rank(level, TEST_NODE_RANK);
        printf("  Level %d: Partner rank=%d, ", level, partner_rank);
        print_mac("Partner MAC", partner_mac);
    }
    printf("  Level %d (final): Should use stored Level 0 source MAC (0x%012lx)\n", 
           MAX_RECURSION_LEVEL + 1, (unsigned long)TEST_PYTORCH_MAC);
    printf("\nNote: Hardware debug prints in NIC.scala will show actual MACs used:\n");
    printf("  - [EthernetHeaderExtractor] shows source MAC from incoming packets\n");
    printf("  - [EthernetHeaderPrepender] shows destination MAC for outgoing packets\n");
    printf("  - Module prints show routing decisions and expected partner MACs\n");
    printf("Cross-reference hardware prints with expected values above.\n\n");
    #endif

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

        // --- Flush any pending receive completions from NIC initialization ---
        // This ensures we don't process any garbage data that might be in the NIC buffers
        while (nic_recv_comp_avail() > 0) {
            // Acknowledge and discard any pending completions
            reg_read16(SIMPLENIC_RECV_COMP);
            asm volatile ("fence");
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

                // Construct TX Packet with Ethernet header
                memset(tx_buf, 0, BUF_SIZE);
                
                // --- Ethernet Header (16 bytes: 14 bytes header + 2 bytes padding) ---
                // For Level 0 packets: from PyTorch (TEST_PYTORCH_MAC) to this node
                // For Level 1-3 packets: from this node to partner (simulated)
                uint64_t src_mac, dst_mac;
                if (level == 0) {
                    // Level 0: from PyTorch to this node
                    src_mac = TEST_PYTORCH_MAC;
                    dst_mac = BASE_MAC | TEST_NODE_RANK;
                } else {
                    // Level 1-3: from this node to partner (simulated intermediate packet)
                    src_mac = BASE_MAC | TEST_NODE_RANK;
                    uint8_t partner_rank = calculate_partner_rank(level, TEST_NODE_RANK);
                    dst_mac = BASE_MAC | partner_rank;
                }
                
                // Ethernet header wire format: [padding(2B) | dstmac(6B) | srcmac(6B) | ethType(2B)]
                // For 64-bit words: word0 = [padding(2B) | dstmac(6B)], word1 = [srcmac(6B) | ethType(2B)]
                // The extractor does Cat(words.reverse), so word1 comes first, then word0
                // When bytes are written sequentially and read as 64-bit words in little-endian:
                //   Word = byte[0] | (byte[1] << 8) | ... | (byte[7] << 56)
                // So byte[0] is LSB, byte[7] is MSB of the word
                
                // Word 0 (bytes 0-7): padding(2) + dstmac(6)
                // Write in little-endian: LSB first
                tx_buf[0] = 0x00;   // Padding byte 0 (LSB of word 0)
                tx_buf[1] = 0x00;   // Padding byte 1
                for (int i = 0; i < 6; i++) {
                    tx_buf[2 + i] = (uint8_t)((dst_mac >> (i * 8)) & 0xFF);  // dstmac bytes 0-5 (LSB to MSB)
                }
                
                // Word 1 (bytes 8-15): srcmac(6) + ethType(2)
                // Write in little-endian: LSB first
                for (int i = 0; i < 6; i++) {
                    tx_buf[8 + i] = (uint8_t)((src_mac >> (i * 8)) & 0xFF);  // srcmac bytes 0-5 (LSB to MSB)
                }
                tx_buf[14] = 0x00;  // EtherType byte 0 (LSB)
                tx_buf[15] = 0x00;  // EtherType byte 1 (MSB)
                
                // --- Metadata (16 bytes, starting at offset 16) ---
                uint8_t *metadata = tx_buf + ETH_HEADER_LEN;
                metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                metadata[2] = META_COLL_TYPE;
                metadata[3] = META_OP;
                metadata[4] = 0x00;
                metadata[5] = TEST_NODE_RANK;  // Sender rank in reserved byte
                metadata[6] = MAX_RECURSION_LEVEL;
                metadata[7] = (uint8_t)level;

                // --- Word 1 (Offset 8 in metadata, 24 total): Chunked Metadata ---
                uint32_t chunk_index = (uint32_t)chunk;
                memcpy(metadata + 8, &chunk_index, sizeof(uint32_t));
                memcpy(metadata + 12, &total_chunks, sizeof(uint32_t));

                // --- Data Payload (starting at offset 32) ---
                memcpy(tx_buf + ETH_HEADER_LEN + METADATA_LEN, input_elements[level][chunk], DATA_PAYLOAD_LEN);

                // Verify MAC addresses in buffer before sending (especially for first packet)
                uint64_t tx_src_mac_check = 0, tx_dst_mac_check = 0;
                for (int i = 0; i < 6; i++) {
                    tx_dst_mac_check |= ((uint64_t)tx_buf[2 + i]) << (i * 8);
                    tx_src_mac_check |= ((uint64_t)tx_buf[8 + i]) << (i * 8);
                }
                if (packets_sent == 0) {
                    printf("FIRST PACKET TX: Level %d, src_mac=0x%012lx, dst_mac=0x%012lx (expected src=0x%012lx, dst=0x%012lx)\n",
                           level, (unsigned long)tx_src_mac_check, (unsigned long)tx_dst_mac_check,
                           (unsigned long)src_mac, (unsigned long)dst_mac);
                    if (tx_src_mac_check != src_mac || tx_dst_mac_check != dst_mac) {
                        printf("ERROR: MAC mismatch in TX buffer! Buffer corrupted?\n");
                    }
                }

                nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Sent Packet Details ---\n");
                    print_packet_metadata("TX", tx_buf + ETH_HEADER_LEN);
                    print_elements_f("TX", (const uint32_t*)(tx_buf + ETH_HEADER_LEN + METADATA_LEN), 8);
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

                // --- Extract and verify Ethernet header MAC addresses ---
                // Ethernet header format: [padding(2B) | dstmac(6B) | srcmac(6B) | ethType(2B)]
                // Extract MAC addresses (little-endian byte order)
                uint64_t rx_dst_mac = 0, rx_src_mac = 0;
                for (int i = 0; i < 6; i++) {
                    rx_dst_mac |= ((uint64_t)rx_buf[2 + i]) << (i * 8);  // dstmac bytes 2-7
                    rx_src_mac |= ((uint64_t)rx_buf[8 + i]) << (i * 8);  // srcmac bytes 8-13
                }
                
                // --- Strip Ethernet header from received packet ---
                // The prepender adds an Ethernet header to outgoing packets, so we need to skip it
                uint8_t *rx_payload = rx_buf + ETH_HEADER_LEN;
                
                // --- Extract response level from metadata ---
                uint8_t response_level = rx_payload[7];
                uint32_t response_chunk_index, response_total_chunks;
                memcpy(&response_chunk_index, rx_payload + 8, sizeof(uint32_t));
                memcpy(&response_total_chunks, rx_payload + 12, sizeof(uint32_t));

                #if VERIFY_MAC_ROUTING
                // Verify MAC addresses based on response level
                // In single-node Verilator loopback test:
                // - Packets we send loop back to us
                // - Source MAC is always this node's MAC (because we sent it)
                // - Destination MAC is where we originally sent it (partner's MAC for level 1-3)
                uint64_t this_node_mac = BASE_MAC | TEST_NODE_RANK;
                uint64_t expected_src_mac = this_node_mac;  // Always our MAC in loopback
                uint64_t expected_dst_mac;
                
                if (response_level == MAX_RECURSION_LEVEL + 1) {
                    // Level 4 (final): sent back to Level 0 source (PyTorch MAC)
                    expected_dst_mac = TEST_PYTORCH_MAC;
                } else {
                    // Level 1-3: sent to partner node
                    uint8_t partner_rank = calculate_partner_rank(response_level - 1, TEST_NODE_RANK);
                    expected_dst_mac = BASE_MAC | partner_rank;
                }
                
                // Verify source MAC is this node's MAC (loopback behavior)
                if (rx_src_mac != expected_src_mac) {
                    printf("ERROR: MAC address mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                    printf("  Expected source MAC: 0x%012lx (this node in loopback)\n", (unsigned long)expected_src_mac);
                    printf("  Received source MAC: 0x%012lx\n", (unsigned long)rx_src_mac);
                    printf("  Expected dest MAC:   0x%012lx\n", (unsigned long)expected_dst_mac);
                    printf("  Received dest MAC:   0x%012lx\n", (unsigned long)rx_dst_mac);
                    sim_fail(500 + test_set);
                }
                
                // Verify destination MAC matches where we sent it
                if (rx_dst_mac != expected_dst_mac) {
                    printf("ERROR: Destination MAC mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                    printf("  Expected dest MAC: 0x%012lx\n", (unsigned long)expected_dst_mac);
                    printf("  Received dest MAC: 0x%012lx\n", (unsigned long)rx_dst_mac);
                    sim_fail(501 + test_set);
                }
                
                #if DEBUG_PRINT_PACKETS
                printf("MAC Verification: src=0x%012lx (this node) ✓, dst=0x%012lx (expected=0x%012lx) ✓\n", 
                       (unsigned long)rx_src_mac, (unsigned long)rx_dst_mac, (unsigned long)expected_dst_mac);
                #endif
                #endif

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
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                uint8_t *expected_metadata = expected_rx_buf;
                expected_metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                expected_metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                expected_metadata[2] = META_COLL_TYPE;
                expected_metadata[3] = META_OP;
                expected_metadata[4] = 0x00;
                expected_metadata[5] = 0x00;  // Sender rank (from partner, not verified in this test)
                expected_metadata[6] = MAX_RECURSION_LEVEL;
                expected_metadata[7] = response_level;

                // --- Word 1 (Offset 8): Expected Chunked Metadata ---
                memcpy(expected_metadata + 8, &response_chunk_index, sizeof(uint32_t));
                memcpy(expected_metadata + 12, &response_total_chunks, sizeof(uint32_t));

                // Copy expected chunk data
                memcpy(expected_rx_buf + METADATA_LEN, expected_outputs[response_level-1][response_chunk_index], DATA_PAYLOAD_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Received Packet Details ---\n");
                    print_packet_metadata("RX", rx_payload);
                    print_elements_f("RX", (const uint32_t*)(rx_payload + METADATA_LEN), 16);

                    printf("\n--- Expected Packet Details ---\n");
                    print_packet_metadata("Expected", expected_rx_buf);
                    print_elements_f("Expected", (const uint32_t*)(expected_rx_buf + METADATA_LEN), 16);
                #endif

                // Verify response
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                // 1) Strictly compare metadata
                if (memcmp(rx_payload, expected_rx_buf, METADATA_LEN) != 0) {
                    printf("ERROR: Metadata mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                    sim_fail(400 + test_set);
                }

                // 2) Compare payload with 1-ULP tolerance per 32-bit float element
                {
                    const uint32_t* exp_words = (const uint32_t*)(expected_rx_buf + METADATA_LEN);
                    const uint32_t* got_words = (const uint32_t*)(rx_payload + METADATA_LEN);
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

