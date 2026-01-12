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
    fflush(stdout);
    tohost = 1; // Standard encoding for success
    while (1);
}

static inline void sim_fail(uint64_t code) {
    printf("ERROR: Test FAILED with code %lu. Signaling simulation failure.\n", (unsigned long)code);
    fflush(stdout);
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
#define LEVEL0_MAX_VAL 1000.0f

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)
#define NUM_TEST_SETS  1024       // Single test set for 8-node multi-node test
#define TOTAL_CHUNKS 1024        // Number of chunks to test (adjustable)
#define MAX_CHUNKS_PER_LEVEL 1024  // Maximum supported chunks per level
#define MAX_CHUNK_SPREAD 32        // Max shuffle distance for packet ordering (0 = sequential)
#define NUM_NODES 8             // Number of nodes in the 8-node test

#if MAX_CHUNK_SPREAD < 0
#error "MAX_CHUNK_SPREAD must be non-negative"
#endif

// Define metadata values (example)
#define META_COLL_ID   0xABCD
#define META_COLL_TYPE 0x01
#define META_OP        0x05 // e.g., 5 means ADD
#define META_OP_SETUP     0xFE // Setup Packet to configure Node Rank

#define DEBUG_PRINT_PACKETS 0 // Set to 1 to print full TX/RX packets, 0 to disable

#define BASE_MAC 0x00126D000000ULL
#define TESTER_MAC_OFFSET 0x02
#define ACCELERATOR_MAC_OFFSET 0x22

// MAC byte order macro: Currently uses HOST ORDER (LSB first, little-endian)
// MAC 0x00126D000022 writes as bytes: 22 00 00 6D 12 00 (LSB at lowest address)
//
// To switch to NETWORK ORDER (MSB first, big-endian, standard Ethernet):
//   Change (_i * 8) to ((5 - _i) * 8) in the shift expression below.
//   Network order would write: 00 12 6D 00 00 22 (MSB at lowest address)
#define WRITE_MAC_TO_BUF(buf, offset, mac) do { \
    for (int _i = 0; _i < 6; _i++) { \
        (buf)[(offset) + _i] = (uint8_t)(((mac) >> ((5 - _i) * 8)) & 0xFF); \
    } \
} while(0)

// Alternative: Use compile-time define for node rank
// Build with: -DNODE_RANK=0, -DNODE_RANK=1, etc. for each node
#ifndef NODE_RANK
#define NODE_RANK 0  // Default to 0 if not defined
#endif


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

static inline uint32_t lcg_step(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static inline int bounded_random(uint32_t* state, int upper_bound) {
    if (upper_bound <= 1) {
        return 0;
    }
    return (int)(lcg_step(state) % (uint32_t)upper_bound);
}

static inline float uniform_float(uint32_t* state, float max_val) {
    const float scale = 1.0f / 4294967295.0f;
    uint32_t raw = lcg_step(state);
    return (float)raw * scale * max_val;
}

static void fill_node_chunk_data(uint32_t node,
                                 uint32_t chunk,
                                 float* dst_f,
                                 uint32_t* dst_u32) {
    uint32_t rng_state = 0xC0FFEE00u ^ (node * 0x9E3779B1u) ^ (chunk * 0x7F4A7C15u);
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        float val = uniform_float(&rng_state, LEVEL0_MAX_VAL);
        dst_f[i] = val;
        memcpy(&dst_u32[i], &val, sizeof(uint32_t));
    }
}

/**
 * Generate packet ordering with controlled randomness using MAX_CHUNK_SPREAD.
 * Chunks are sent roughly in order but with random shuffling within a sliding window.
 * 
 * Example with spread=8:
 *   Acceptable: [0, 3, 1, 2, 5, 4, 7, 6, 8, ...]  (shuffled within ±8)
 *   Not allowed: [0, 100, 3, 245, ...]  (wild jumps)
 */
static void generate_spread_limited_order(int* order, int total_chunks, uint32_t seed) {
    if (!order || total_chunks <= 0) return;
    
    uint32_t rng_state = seed ? seed : 0xC001C0DEu;
    int remaining[MAX_CHUNKS_PER_LEVEL];
    int remaining_count = total_chunks;
    
    // Initialize remaining chunks
    for (int i = 0; i < total_chunks; i++) {
        remaining[i] = i;
    }
    
    for (int out_idx = 0; out_idx < total_chunks; out_idx++) {
        // Find minimum chunk still remaining
        int min_chunk = remaining[0];
        for (int i = 1; i < remaining_count; i++) {
            if (remaining[i] < min_chunk) {
                min_chunk = remaining[i];
            }
        }
        
        // Find candidates within spread of minimum
        int chunk_cutoff = min_chunk + MAX_CHUNK_SPREAD;
        int candidate_indices[MAX_CHUNKS_PER_LEVEL];
        int candidate_count = 0;
        for (int i = 0; i < remaining_count; i++) {
            if (remaining[i] <= chunk_cutoff) {
                candidate_indices[candidate_count++] = i;
            }
        }
        
        // Select random candidate from eligible set
        int chosen_idx = candidate_indices[bounded_random(&rng_state, candidate_count)];
        order[out_idx] = remaining[chosen_idx];
        
        // Remove chosen chunk from remaining
        remaining_count--;
        if (chosen_idx != remaining_count) {
            remaining[chosen_idx] = remaining[remaining_count];
        }
    }
}

// Add these debug print functions after the existing helper functions
void print_packet_metadata(const char* prefix, const uint8_t* buf) {
    printf("%s Metadata (16 bytes):\n", prefix);
    
    // --- Word 0 ---
    printf("  Collective ID: 0x%04x\n", (buf[1] << 8) | buf[0]);
    printf("  Collective Type: 0x%02x\n", buf[2]);
    printf("  Operation: 0x%02x\n", buf[3]);
    printf("  Reserved[4]: 0x%02x\n", buf[4]);  // Reserved byte
    printf("  Reserved[5]: 0x%02x\n", buf[5]);  // Reserved (contains rank in Setup packets only)
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

// --- Helper: Send Setup+Warmup Packet (Combined Rank Config + ACK) ---
// Sends Setup packet (opCode=0xFE) with collID=0xFFFF to request ACK.
// Returns 1 on success (ACK received with correct rank), 0 on failure (timeout).
int send_setup_packet(int rank) {
   printf("Sending Setup+Warmup Packet to accelerator (Setting Rank=%d)...\n", rank);
   
   // Buffers
   static uint8_t tx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
   static uint8_t rx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
   
   uint64_t tester_mac_for_setup = BASE_MAC | ((uint64_t)(rank + TESTER_MAC_OFFSET));
   uint64_t accel_mac_for_setup = BASE_MAC | ((uint64_t)(rank + ACCELERATOR_MAC_OFFSET));

   memset(tx_buf_setup, 0, TOTAL_PACKET_LEN);
   
   // Ethernet header: [padding(2)] [dst(6)] [src(6)] [ethtype(2)]
   tx_buf_setup[0] = 0x00; tx_buf_setup[1] = 0x00;  // Padding
   WRITE_MAC_TO_BUF(tx_buf_setup, 2, accel_mac_for_setup);   // Dst = accel
   WRITE_MAC_TO_BUF(tx_buf_setup, 8, tester_mac_for_setup);  // Src = tester
   tx_buf_setup[14] = 0x00; tx_buf_setup[15] = 0x00;  // EtherType
   
   // Metadata
   uint8_t *meta = tx_buf_setup + ETH_HEADER_LEN;
   meta[0] = 0xFF; meta[1] = 0xFF;  // CollID = 0xFFFF (triggers ACK)
   meta[2] = 0;                      // Type
   meta[3] = META_OP_SETUP;          // Operation: SETUP (0xFE)
   meta[5] = (uint8_t)rank;          // New Rank to configure (Setup packets only)
   
   // Pre-post receive buffer for ACK
   reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
   
   // Send Setup+Warmup packet
   nic_send(tx_buf_setup, TOTAL_PACKET_LEN);
   
   // Wait for Setup ACK
   printf("Waiting for Setup ACK...\n");
   uint64_t timeout = 20000000;  // ~20ms at 1GHz
   
   while (timeout > 0) {
       if (nic_recv_comp_avail() > 0) {
           reg_read16(SIMPLENIC_RECV_COMP);
           asm volatile ("fence");
           
           uint8_t *rx_meta = rx_buf_setup + ETH_HEADER_LEN;
           uint16_t coll_id = rx_meta[0] | (rx_meta[1] << 8);
           uint8_t op_code = rx_meta[3];
           uint8_t response_level = rx_meta[7];
           
           // Print full ACK metadata including source MAC
           // Note: byte 5 is reserved (no longer contains sender rank)
           printf("ACK Packet: collID=0x%04x, opCode=0x%02x, level=%u, srcMAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   coll_id, op_code, response_level,
                   rx_buf_setup[8], rx_buf_setup[9], rx_buf_setup[10],
                   rx_buf_setup[11], rx_buf_setup[12], rx_buf_setup[13]);
           
           // Check for Setup ACK: collID=0xFFFF, opCode=0xFE, level=1
           if (coll_id == 0xFFFF && op_code == META_OP_SETUP && response_level == 1) {
               printf("Setup ACK received! Rank=%d confirmed by ACK response.\n", rank);
               return 1;  // Success
           } else {
               printf("Ignoring non-ACK packet, continuing wait...\n");
               // Re-post buffer and continue waiting
               reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
           }
       }
       timeout--;
   }
   
   printf("ERROR: Setup ACK timeout! Accelerator may not have configured rank correctly.\n");
   return 0;  // Failure
}

// --- Main Test ---

int main(int argc, char *argv[]) {

    // Suppress unused function warning for nic_recv (we use async recv instead)
    (void)nic_recv;

    printf("Starting 8-Node RecursiveDoubling Test (Level 0 -> Level 4)...\n");
    printf("This node will send Level 0 packets and wait for Level 4 responses\n");
    
    // --- 1. NIC Initialization and Node Rank Derivation ---
    printf("SimpleNIC assumed ready after reset.\n");
    
    // Strategy: Use compile-time define NODE_RANK (set via -DNODE_RANK=X when building)
    // This allows us to build 8 binaries from the same source, each with a different rank
    uint8_t TEST_NODE_RANK = NODE_RANK;
    
    // Validate and clamp node rank
    if (TEST_NODE_RANK >= NUM_NODES) {
        printf("WARNING: Node rank %u >= NUM_NODES (%d), using rank 0\n", TEST_NODE_RANK, NUM_NODES);
        TEST_NODE_RANK = 0;
    }
    
    uint64_t tester_mac = BASE_MAC | ((uint64_t)(TEST_NODE_RANK + TESTER_MAC_OFFSET));
    uint64_t accel_mac  = BASE_MAC | ((uint64_t)(TEST_NODE_RANK + ACCELERATOR_MAC_OFFSET));
    
    printf("Node Rank: %u (from compile-time -DNODE_RANK=%u)\n", TEST_NODE_RANK, NODE_RANK);
    printf("NIC (tester) MAC: %012lx\n", (unsigned long)tester_mac);
    printf("Accelerator MAC: %012lx\n", (unsigned long)accel_mac);
    printf("Note: Accelerator will learn its nodeRank from the destination MAC of the first packet.\n");

    reg_write64(SIMPLENIC_MACADDR, tester_mac);
    asm volatile ("fence");   // optional, keeps the write ordered
    
    printf("Running %d test set(s) with %d node(s)\n", NUM_TEST_SETS, NUM_NODES);
    printf("Each set: %d elements (%d bytes payload per chunk)\n",
           NUM_ELEMENTS, DATA_PAYLOAD_LEN);
    printf("Max Recursion Level: %d\n", MAX_RECURSION_LEVEL);
    #if DEBUG_PRINT_PACKETS
        printf(">>> Full packet debug printing is ENABLED <<<\n");
    #else
        printf(">>> Full packet debug printing is DISABLED <<<\n");
    #endif

    // Allocate buffers (static for bare-metal)
    // Ensure alignment for potential DMA requirements by NIC
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    // 4-buffer ring for RX to handle bursty packet arrivals
    // With 4 buffers: 1 being processed, 3 in NIC queue = handles bursts of up to 4 packets
    #define NUM_RX_BUFFERS (MAX_CHUNK_SPREAD*2)
    static uint8_t rx_buffers[NUM_RX_BUFFERS][BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t *rx_buf = NULL;  // Points to current buffer being processed
    static int rx_tail = 0;  // Next buffer to be processed by CPU (FIFO order)
    static uint8_t rx_local_copy[BUF_SIZE] __attribute__((aligned(64)));  // Safe local copy of received packet
    static uint8_t expected_rx_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Buffers to hold the chunked input data payloads (as 32-bit elements)
    // Use float for easier calculation, then cast to uint32_t for transmission
    static float input_elements_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static float expected_outputs_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t input_elements[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t expected_outputs[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static float temp_node_data[NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t temp_node_words[NUM_ELEMENTS] __attribute__((aligned(64)));

    // =========================================================================
    // Combined Setup+Warmup (replaces old Phases 0, 1, and 2)
    // =========================================================================
    // The combined approach:
    // 1. Sends Setup packet (opCode=0xFE) with collID=0xFFFF to configure rank
    // 2. Waits for hardware ACK response to confirm rank is set
    // 3. ACK response also serves as warmup (flushes pipeline, registers MACs)
    // =========================================================================
    {
        printf("=== Combined Setup+Warmup ===\n");
        
        if (!send_setup_packet(TEST_NODE_RANK)) {
            printf("CRITICAL: Setup+Warmup failed! Continuing anyway but expect issues.\n");
        } else {
            printf("Setup+Warmup successful! Rank=%d confirmed, pipeline flushed, MACs registered.\n", TEST_NODE_RANK);
        }
        // No delay/drain needed - send_setup_packet already waits for ACK response
    }
    // =========================================================================

    // --- 2. Run Test Set (8-Node Multi-Node Test) ---
    for (int test_set = 0; test_set < NUM_TEST_SETS; test_set++) {
        printf("\n=== Starting 8-Node Test Set %d ===\n", test_set + 1);

        // Generate unique collective ID for this test set
        uint16_t test_collective_id = (uint16_t)(0x1000 + test_set);
        printf("Collective ID for this set: 0x%04X\n", test_collective_id);

        // For 8-node test: can test with multiple chunks, but only send Level 0
        // Start with a reasonable number of chunks for initial testing
        uint32_t total_chunks = TOTAL_CHUNKS;  // Controlled by global define
        printf("Testing with %u chunk(s) per level\n", total_chunks);
        printf("8-Node Test Mode: Each node sends Level 0 packets only (all chunks), receives Level 4\n");
        
        // Generate packet order with controlled randomness (spread-limited shuffling)
        int level0_packet_order[MAX_CHUNKS_PER_LEVEL];
        uint32_t schedule_seed = 0xBADC0DEu ^ (uint32_t)test_set ^ ((uint32_t)NODE_RANK << 16);
        generate_spread_limited_order(level0_packet_order, total_chunks, schedule_seed);
        
        printf("Generated packet order for %u chunks (spread=%d)\n", 
               total_chunks, MAX_CHUNK_SPREAD);
        
        // Prepare deterministic Level 0 input data for this node
        for (int chunk = 0; chunk < total_chunks; ++chunk) {
            fill_node_chunk_data(TEST_NODE_RANK, chunk, input_elements_f[0][chunk], input_elements[0][chunk]);
        }

        // Calculate expected Level 4 output = sum of all nodes' Level 0 data
        for (int chunk = 0; chunk < total_chunks; ++chunk) {
            for (int i = 0; i < NUM_ELEMENTS; ++i) {
                expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i] = 0.0f;
            }

            for (int node = 0; node < NUM_NODES; ++node) {
                fill_node_chunk_data(node, chunk, temp_node_data, temp_node_words);
                for (int i = 0; i < NUM_ELEMENTS; ++i) {
                    expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i] += temp_node_data[i];
                }
            }

            memcpy(expected_outputs[MAX_RECURSION_LEVEL][chunk],
                   expected_outputs_f[MAX_RECURSION_LEVEL][chunk],
                   DATA_PAYLOAD_LEN);
        }
        
        printf("Expected Level 4 result: sum of all %d nodes' Level 0 data (each node has different data)\n", NUM_NODES);

        // --- 8-Node Test: Send Level 0, Receive Level 4 ---
        printf("\n--- Sending Level 0 packets and waiting for Level 4 responses ---\n");
        // Only send Level 0 packets (one per chunk)
        int total_packets_to_send = total_chunks;  // Only Level 0
        // Only expect Level 4 responses (one per chunk)
        int total_expected_responses = total_chunks;  // Only Level 4
        int responses_received = 0;
        int packets_sent = 0;
        int received_chunks_level4[MAX_CHUNKS_PER_LEVEL]; // Track which Level 4 chunks we've received

        // A stall detector
        const uint64_t STALL_TIMEOUT_CYCLES = 100000000; // Longer timeout for multi-node
        uint64_t stall_counter = 0;

        // Initialize tracking array (only for Level 4)
        for (int c = 0; c < MAX_CHUNKS_PER_LEVEL; c++) {
            received_chunks_level4[c] = 0;
        }

        // --- Flush any pending receive completions from NIC initialization ---
        // This ensures we don't process any garbage data that might be in the NIC buffers
        while (nic_recv_comp_avail() > 0) {
            // Acknowledge and discard any pending completions
            reg_read16(SIMPLENIC_RECV_COMP);
            asm volatile ("fence");
        }
        
        // --- Clear ring buffers to prevent reading stale data from previous test set ---
        for (int i = 0; i < NUM_RX_BUFFERS; i++) {
            memset(rx_buffers[i], 0, BUF_SIZE);
        }
        
        // --- Arm the NIC by pre-posting all receive buffers (first test set only) ---
        // For subsequent test sets, the 32 buffers are already posted from re-posts
        rx_tail = 0;
        if (test_set == 0) {
            printf("Pre-posting %d receive buffers for ring buffer.\n", NUM_RX_BUFFERS);
            for (int i = 0; i < NUM_RX_BUFFERS; i++) {
                while (nic_recv_req_avail() == 0); // Wait until NIC can accept
                reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buffers[i]);
            }
        } else {
            printf("Reusing %d already-posted receive buffers.\n", NUM_RX_BUFFERS);
        }


        // --- Main polling loop (8-Node: Level 0 TX, Level 4 RX) ---
        while (responses_received < total_expected_responses) {
            
            // === 1. SEND LEVEL 0 PACKETS ===
            // Can we send? (NIC has buffer space AND we have Level 0 packets left to send)
            if (nic_send_req_avail() > 0 && packets_sent < total_packets_to_send) {
                // In 8-node test, we only send Level 0 packets
                // Use constrained packet order for proper interleaving when testing with multiple chunks
                int packet_index = level0_packet_order[packets_sent];
                int chunk = packet_index;  // Packet order is just chunk indices for Level 0
                int level = 0;  // Always Level 0

                #if DEBUG_PRINT_PACKETS
                    printf("Sending Level 0 Packet %d/%d (order[%d]=%d): Chunk %d\n", 
                           packets_sent+1, total_packets_to_send, packets_sent, packet_index, chunk);
                #endif

                // Construct TX Packet WITH Ethernet header
                // NOTE: The NIC does NOT add Ethernet headers automatically - we must include them!
                // The EthernetHeaderPrepender only processes packets from the accelerator module,
                // NOT packets sent directly from the CPU via nic_send().
                memset(tx_buf, 0, BUF_SIZE);
                
                // --- Ethernet Header (16 bytes) ---
                // Format: [padding(2B) | dstmac(6B) | srcmac(6B) | ethType(2B)]
                
                // For Level 0 packets: tester -> accelerator (distinct MACs to keep software isolated)
                uint64_t src_mac = tester_mac;
                uint64_t dst_mac = accel_mac;
                
                // Padding (2 bytes)
                tx_buf[0] = 0x00;
                tx_buf[1] = 0x00;
                
                // Destination MAC (6 bytes)
                WRITE_MAC_TO_BUF(tx_buf, 2, dst_mac);
                
                // Source MAC (6 bytes)
                WRITE_MAC_TO_BUF(tx_buf, 8, src_mac);
                
                // EtherType (2 bytes)
                tx_buf[14] = 0x00;
                tx_buf[15] = 0x00;
                
                // --- Metadata (16 bytes, starting at offset 16 after Ethernet header) ---
                uint8_t *metadata = tx_buf + ETH_HEADER_LEN;
                metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                metadata[2] = META_COLL_TYPE;
                metadata[3] = META_OP;
                metadata[4] = 0;  // Reserved
                metadata[5] = 0;  // Reserved (rank only used in Setup packets)
                metadata[6] = MAX_RECURSION_LEVEL;
                metadata[7] = (uint8_t)level;  // Level 0

                // --- Word 1 (Offset 8 in metadata): Chunked Metadata ---
                uint32_t chunk_index = (uint32_t)chunk;
                memcpy(metadata + 8, &chunk_index, sizeof(uint32_t));
                memcpy(metadata + 12, &total_chunks, sizeof(uint32_t));

                // --- Data Payload (starting at offset 32, after Ethernet header + metadata) ---
                memcpy(tx_buf + ETH_HEADER_LEN + METADATA_LEN, input_elements[0][chunk], DATA_PAYLOAD_LEN);

                // Send full packet including Ethernet header
                nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Sent Level 0 Packet Details (with Ethernet header) ---\n");
                    printf("Ethernet: src=0x%012lx, dst=0x%012lx (tester -> accelerator)\n", 
                           (unsigned long)src_mac, (unsigned long)dst_mac);
                    print_packet_metadata("TX Level 0", metadata);
                    print_elements_f("TX Level 0", (const uint32_t*)(tx_buf + ETH_HEADER_LEN + METADATA_LEN), 8);
                #endif

                packets_sent++;
                stall_counter = 0; // Reset stall counter because we made progress
            }

            // === 2. POLL FOR LEVEL 4 RESPONSES ===
            // Has the NIC filled our pre-posted buffer?
            if (nic_recv_comp_avail() > 0) {
                // Consume from ring buffer tail (completions are FIFO - buffers filled in order posted)
                rx_buf = rx_buffers[rx_tail];
                rx_tail = (rx_tail + 1) % NUM_RX_BUFFERS;
                
                // Acknowledge the completion and get the packet length
                int received_len = reg_read16(SIMPLENIC_RECV_COMP);
                asm volatile ("fence");
                
                // IMMEDIATELY copy packet to local buffer before NIC can overwrite it
                // This prevents race condition where NIC fills rx_buf with next packet
                memcpy(rx_local_copy, rx_buf, BUF_SIZE);
                
                // Verify that the packet length matches expected payload length
                if (received_len != TOTAL_PACKET_LEN) {
                    printf("ERROR: Received packet with unexpected length! Expected %d, Got %d\n",
                        TOTAL_PACKET_LEN, received_len);
                    sim_fail(600 + test_set);
                }

                // Use local copy for all processing (safe from NIC overwrites)
                uint8_t *rx_payload = rx_local_copy + ETH_HEADER_LEN;
                
                // --- Extract response level from metadata ---
                uint8_t response_level = rx_payload[7];
                uint32_t response_chunk_index, response_total_chunks;
                memcpy(&response_chunk_index, rx_payload + 8, sizeof(uint32_t));
                memcpy(&response_total_chunks, rx_payload + 12, sizeof(uint32_t));


                #if DEBUG_PRINT_PACKETS
                    printf("Received packet: level=%u, chunk=%u, reserved5=0x%02x\n", 
                           response_level, response_chunk_index, rx_payload[5]);
                #endif
                
                // Ignore intermediate levels (they are routed to other nodes when MAC filtering is correct)
                if (response_level != (MAX_RECURSION_LEVEL + 1)) {
                    #if DEBUG_PRINT_PACKETS
                        printf("Ignoring non-Level-4 packet (level=%u, chunk=%u)\n",
                            response_level, response_chunk_index);
                        print_packet_metadata("RX (non-L4)", rx_payload);
                        print_elements_f("RX (non-L4)", (const uint32_t*)(rx_payload + METADATA_LEN), 8);
                    #endif
                    // Done with this buffer, clear and re-post (always, to maintain NIC queue)
                    memset(rx_buf, 0, BUF_SIZE);  // Clear before re-posting
                    while (nic_recv_req_avail() == 0);
                    reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
                    stall_counter = 0;
                    continue;
                }

                // Check for duplicate Level 4 responses
                if (response_chunk_index >= MAX_CHUNKS_PER_LEVEL) {
                    printf("ERROR: Invalid chunk index %u (max %d)\n", response_chunk_index, MAX_CHUNKS_PER_LEVEL - 1);
                    sim_fail(301 + test_set);
                }
                
                if (received_chunks_level4[response_chunk_index]) {
                    printf("ERROR: Duplicate Level 4 response for chunk %u\n", response_chunk_index);
                    sim_fail(300 + test_set);
                }
                received_chunks_level4[response_chunk_index] = 1;

                // Construct expected Level 4 response
                memset(expected_rx_buf, 0, BUF_SIZE);
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                uint8_t *expected_metadata = expected_rx_buf;
                expected_metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                expected_metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                expected_metadata[2] = META_COLL_TYPE;
                expected_metadata[3] = META_OP;
                expected_metadata[4] = 0;  // Reserved
                expected_metadata[5] = 0;  // Reserved
                expected_metadata[6] = MAX_RECURSION_LEVEL;
                expected_metadata[7] = MAX_RECURSION_LEVEL + 1;  // Expected Level 4 (final) response

                // --- Word 1 (Offset 8): Expected Chunked Metadata ---
                memcpy(expected_metadata + 8, &response_chunk_index, sizeof(uint32_t));
                memcpy(expected_metadata + 12, &response_total_chunks, sizeof(uint32_t));

                // Copy expected Level 4 chunk data (sum of all 8 nodes' Level 0 data)
                memcpy(expected_rx_buf + METADATA_LEN, expected_outputs[MAX_RECURSION_LEVEL][response_chunk_index], DATA_PAYLOAD_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Received Packet Details (reported level %u) ---\n", response_level);
                    print_packet_metadata("RX Packet", rx_payload);
                    print_elements_f("RX Packet", (const uint32_t*)(rx_payload + METADATA_LEN), 8);

                    printf("\n--- Expected Level 4 Packet Details (target level %u, sum of all %d nodes' Level 0) ---\n",
                           MAX_RECURSION_LEVEL + 1, NUM_NODES);
                    print_packet_metadata("Expected Level 4", expected_rx_buf);
                    print_elements_f("Expected Level 4", (const uint32_t*)(expected_rx_buf + METADATA_LEN), 8);
                #endif

                // Verify Level 4 response
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                // 1) Strictly compare metadata
                if (memcmp(rx_payload, expected_rx_buf, METADATA_LEN) != 0) {
                    printf("ERROR: Level 4 metadata mismatch for chunk %u!\n", response_chunk_index);
                    printf("--- Received Metadata ---\n");
                    print_packet_metadata("RX", rx_payload);
                    printf("--- Expected Metadata ---\n");
                    print_packet_metadata("Expected", expected_rx_buf);
                    printf("--- Received Data (first 8 elements) ---\n");
                    print_elements_f("RX Data", (const uint32_t*)(rx_payload + METADATA_LEN), 8);
                    printf("--- Expected Data (first 8 elements) ---\n");
                    print_elements_f("Exp Data", (const uint32_t*)(expected_rx_buf + METADATA_LEN), 8);
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

                        if (udiff > 3u) {
                            bad_elem = e;
                            bad_exp = a;
                            bad_got = b;
                            bad_diff = udiff;
                            break;
                        }
                    }

                    if (bad_elem >= 0) {
                        printf("ERROR: Level 4 float payload mismatch for chunk %u!\n", response_chunk_index);
                        printf("  First differing element %d: Expected 0x%08x Got 0x%08x (ULP diff %u)\n",
                               bad_elem, bad_exp, bad_got, bad_diff);
                        printf("  Expected = sum of all %d nodes' Level 0 data\n", NUM_NODES);
                        sim_fail(400 + test_set);
                    }
                }

                #if DEBUG_PRINT_PACKETS
                    printf("Level 4 response for chunk %u verified successfully (sum of all %d nodes' Level 0).\n", 
                           response_chunk_index, NUM_NODES);
                #endif
                
                responses_received++;
                stall_counter = 0; // Reset stall counter because we made progress
                
                // Clear and re-post this buffer (always, to maintain NIC queue)
                memset(rx_buf, 0, BUF_SIZE);  // Clear before re-posting
                while (nic_recv_req_avail() == 0); // Wait for space
                reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
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

        printf("All %d Level 0 packets sent and %d Level 4 responses received.\n", packets_sent, responses_received);
        printf("=== 8-Node Test Set %d Completed Successfully ===\n", test_set + 1);
    }

    printf("\n--- 8-Node Test Completed Successfully ---\n");
    printf("All nodes sent Level 0 packets and received correct Level 4 results.\n");
    sim_pass();
    return 0;
}

