#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // For size_t, NULL etc.
#include <time.h>

// --- MMIO and NIC Driver Headers ---
#include "../../mmio.h" // Assumed to be in ../ relative to the C file
#include "../../nic.h"  // Assumed to be in ../ relative to the C file

// --- Simulation Control via 'tohost' ---
extern volatile uint64_t tohost;

// Functions to signal simulation pass/fail
static inline void sim_pass() {
    printf("SUCCESS: Test PASSED. Signaling simulation success.\n");
    fflush(stdout);
    tohost = 1; // Standard encoding for success
    while (1);
}

// Add rdcycle() for latency measurement
#include <riscv-pk/encoding.h>

static inline void sim_fail(uint64_t code) {
    printf("ERROR: Test FAILED with code %lu. Signaling simulation failure.\n", (unsigned long)code);
    fflush(stdout);
    if (code == 0) code = 0xFF; // Ensure failure code is non-zero
    tohost = (code << 1) | 1; // Standard encoding for failure
    while (1);
}

// --- Test Configuration ---
#define BUF_SIZE 2048         // Should be >= TOTAL_PACKET_LEN
#define NUM_LEVELS (MAX_RECURSION_LEVEL + 1)
#define DATA_PAYLOAD_LEN 1024 // Fixed payload size
#define ETH_HEADER_LEN 16      // Ethernet header (14 bytes + 2 bytes padding)
#define METADATA_LEN 16        // Fixed metadata size
#define TOTAL_PACKET_LEN (ETH_HEADER_LEN + METADATA_LEN + DATA_PAYLOAD_LEN)  // 16 + 16 + 1024 = 1056

// FP Format codes (matches hardware)
#define FP_FORMAT_FP32     0x00  // 32-bit float: 256 elements/chunk
#define FP_FORMAT_BFLOAT16 0x01  // 16-bit bfloat: 512 elements/chunk
#define FP_FORMAT_DLFLOAT  0x02  // 16-bit DLFloat: 512 elements/chunk

// Number of packed uint32 words in a chunk (always 256 for 1KB payload)
#define NUM_PACKED_WORDS 256
// Maximum elements per chunk (worst case: 16-bit formats = 512)
#define MAX_ELEMENTS_PER_CHUNK 512

#define LEVEL0_MAX_VAL 1000.0f

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)
#define NUM_TEST_SETS  1024      // Test repetitions per (format, chunk_count) config
#define MAX_CHUNKS_PER_LEVEL 1024  // Maximum supported chunks per level
#define MAX_CHUNK_SPREAD 32        // Max shuffle distance for packet ordering (0 = sequential)
#define NUM_NODES 8             // Number of nodes in the 8-node test

// Sweep configurations — run all of these automatically
static const int CHUNK_COUNTS[] = {1, 4, 16, 64, 256, 1024};
#define NUM_CHUNK_CONFIGS 6

static const int FP_FORMATS[]      = {FP_FORMAT_FP32, FP_FORMAT_BFLOAT16, FP_FORMAT_DLFLOAT};
static const char* FP_FORMAT_NAMES[] = {"FP32", "BF16", "DLFloat"};
#define NUM_FP_FORMAT_CONFIGS 3

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

// ============================================================================
// BFloat16 and DLFloat Conversion Functions
// ============================================================================

// Convert float to BFloat16 (round-nearest-even per IEEE 754)
static inline uint16_t float_to_bf16(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t bits = conv.u;
    uint32_t guard = (bits >> 15) & 0x1;
    uint32_t round_sticky = bits & 0x7FFF;
    uint32_t lsb = (bits >> 16) & 0x1;
    uint32_t round_up = guard && (round_sticky || lsb);
    uint16_t result = (uint16_t)(bits >> 16);
    if (round_up) result++;
    return result;
}

static inline float bf16_to_float(uint16_t bf16) {
    union { float f; uint32_t u; } conv;
    conv.u = ((uint32_t)bf16) << 16;
    return conv.f;
}

// Convert float to DLFloat (round-nearest-up per DLFloat spec)
static inline uint16_t float_to_dlfloat(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t bits = conv.u;
    uint32_t sign = (bits >> 31) & 0x1;
    int32_t exp32 = ((bits >> 23) & 0xFF);
    uint32_t mant32 = bits & 0x7FFFFF;
    if (exp32 == 0) return 0x0000;
    if (exp32 == 255) return 0x7FFF;
    int32_t exp_unbiased = exp32 - 127;
    int32_t exp_dlf = exp_unbiased + 31;
    if (exp_dlf <= 0) return 0x0000;
    if (exp_dlf >= 63) return (uint16_t)((sign << 15) | 0x7FFE);
    uint32_t guard = (mant32 >> 13) & 0x1;
    uint32_t mant_dlf = (mant32 >> 14) + guard;
    if (mant_dlf > 0x1FF) {
        mant_dlf = 0;
        exp_dlf++;
        if (exp_dlf >= 63) return (uint16_t)((sign << 15) | 0x7FFE);
    }
    return (uint16_t)((sign << 15) | (exp_dlf << 9) | mant_dlf);
}

static inline float dlfloat_to_float(uint16_t dlf) {
    if (dlf == 0x7FFF || dlf == 0xFFFF) {
        union { float f; uint32_t u; } conv;
        conv.u = 0x7F800000;
        return conv.f;
    }
    if (dlf == 0x0000 || dlf == 0x8000) return 0.0f;
    uint32_t sign = (dlf >> 15) & 0x1;
    uint32_t exp_dlf = (dlf >> 9) & 0x3F;
    uint32_t mant_dlf = dlf & 0x1FF;
    int32_t exp_unbiased = exp_dlf - 31;
    int32_t exp32 = exp_unbiased + 127;
    if (exp32 <= 0) return sign ? -0.0f : 0.0f;
    if (exp32 >= 255) {
        union { float f; uint32_t u; } conv;
        conv.u = (sign << 31) | 0x7F800000;
        return conv.f;
    }
    uint32_t mant32 = mant_dlf << 14;
    union { float f; uint32_t u; } conv;
    conv.u = (sign << 31) | (exp32 << 23) | mant32;
    return conv.f;
}

// Runtime FP format helpers — accept fp_format as param so one binary covers all formats
static inline float rt_roundtrip(float f, int fp_format) {
    if (fp_format == FP_FORMAT_BFLOAT16) return bf16_to_float(float_to_bf16(f));
    if (fp_format == FP_FORMAT_DLFLOAT)  return dlfloat_to_float(float_to_dlfloat(f));
    return f; // FP32: no precision loss
}

static inline uint16_t rt_to_raw16(float f, int fp_format) {
    if (fp_format == FP_FORMAT_BFLOAT16) return float_to_bf16(f);
    if (fp_format == FP_FORMAT_DLFLOAT)  return float_to_dlfloat(f);
    return 0; // Not used for FP32
}

static inline float rt_from_raw16(uint16_t raw, int fp_format) {
    if (fp_format == FP_FORMAT_BFLOAT16) return bf16_to_float(raw);
    if (fp_format == FP_FORMAT_DLFLOAT)  return dlfloat_to_float(raw);
    return 0.0f; // Not used for FP32
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
                                 int fp_format,
                                 int num_elements,
                                 float* dst_f,
                                 uint32_t* dst_u32) {
    uint32_t rng_state = 0xC0FFEE00u ^ (node * 0x9E3779B1u) ^ (chunk * 0x7F4A7C15u);
    for (int i = 0; i < num_elements; ++i) {
        float raw_val = uniform_float(&rng_state, LEVEL0_MAX_VAL);
        dst_f[i] = rt_roundtrip(raw_val, fp_format);
    }

    if (fp_format == FP_FORMAT_FP32) {
        memcpy(dst_u32, dst_f, DATA_PAYLOAD_LEN);
    } else {
        for (int i = 0; i < num_elements; i += 2) {
            uint16_t val0 = rt_to_raw16(dst_f[i],     fp_format);
            uint16_t val1 = rt_to_raw16(dst_f[i + 1], fp_format);
            dst_u32[i / 2] = ((uint32_t)val1 << 16) | (uint32_t)val0;
        }
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
    printf("  TEST_FP_FORMAT: 0x%02x\n", buf[4]);  // TEST_FP_FORMAT
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

// Helper function to print float elements (debug mode only)
void print_elements_f(const char* title, const uint32_t* elements, size_t num_elements, int fp_format) {
    printf("%s (%lu words):\n", title, (unsigned long)num_elements);
    if (fp_format == FP_FORMAT_FP32) {
        // FP32: each uint32 is one float
        for (size_t i = 0; i < num_elements; ++i) {
            float val;
            memcpy(&val, &elements[i], sizeof(float));
            printf("%12.4f ", val);
            if ((i + 1) % 8 == 0 || i == num_elements - 1) printf("\n");
        }
    } else {
        // 16-bit formats: each uint32 contains 2 packed 16-bit values
        size_t count = 0;
        for (size_t i = 0; i < num_elements; ++i) {
            uint16_t low  = (uint16_t)(elements[i] & 0xFFFF);
            uint16_t high = (uint16_t)((elements[i] >> 16) & 0xFFFF);
            printf("%12.4f ", rt_from_raw16(low,  fp_format)); count++;
            if (count % 8 == 0) printf("\n");
            printf("%12.4f ", rt_from_raw16(high, fp_format)); count++;
            if (count % 8 == 0) printf("\n");
        }
        if (count % 8 != 0) printf("\n");
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
    
    printf("Sweep: %d FP formats x %d chunk configs x %d test sets per config\n",
           NUM_FP_FORMAT_CONFIGS, NUM_CHUNK_CONFIGS, NUM_TEST_SETS);
    printf("Max Recursion Level: %d\n", MAX_RECURSION_LEVEL);
    #if DEBUG_PRINT_PACKETS
        printf(">>> Full packet debug printing is ENABLED <<<\n");
    #else
        printf(">>> Full packet debug printing is DISABLED <<<\n");
    #endif

    // Allocate buffers (static for bare-metal)
    static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(64)));
    #define NUM_RX_BUFFERS (MAX_CHUNK_SPREAD*2)
    static uint8_t rx_buffers[NUM_RX_BUFFERS][BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t *rx_buf = NULL;
    static int rx_tail = 0;
    static uint8_t rx_local_copy[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t expected_rx_buf[BUF_SIZE] __attribute__((aligned(64)));

    // Float arrays: worst-case 512 elements/chunk (16-bit formats)
    static float input_elements_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][MAX_ELEMENTS_PER_CHUNK] __attribute__((aligned(64)));
    static float expected_outputs_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][MAX_ELEMENTS_PER_CHUNK] __attribute__((aligned(64)));
    // uint32 arrays: always 256 packed words/chunk (1KB payload)
    static uint32_t input_elements[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_PACKED_WORDS] __attribute__((aligned(64)));
    static uint32_t expected_outputs[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_PACKED_WORDS] __attribute__((aligned(64)));
    static uint32_t temp_node_words[NUM_PACKED_WORDS] __attribute__((aligned(64)));
    static float all_nodes_data[NUM_NODES][MAX_ELEMENTS_PER_CHUNK];
    static float next_level_data[NUM_NODES][MAX_ELEMENTS_PER_CHUNK];

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

    // --- 2. Full Sweep: FP Format × Chunk Count × Test Sets ---
    // Global collective ID counter — unique across all loops to avoid hardware ID collisions
    uint16_t global_coll_id = 0x1000;

    for (int fmt_idx = 0; fmt_idx < NUM_FP_FORMAT_CONFIGS; fmt_idx++) {
        int fp_format    = FP_FORMATS[fmt_idx];
        int num_elements = (fp_format == FP_FORMAT_FP32) ? 256 : 512;

        printf("\n========================================================\n");
        printf("=== FP FORMAT: %s (%d elements/chunk) ===\n", FP_FORMAT_NAMES[fmt_idx], num_elements);
        printf("========================================================\n");



        for (int chunk_idx = 0; chunk_idx < NUM_CHUNK_CONFIGS; chunk_idx++) {
            uint32_t total_chunks = (uint32_t)CHUNK_COUNTS[chunk_idx];

            printf("\n--- Chunk Count: %u ---\n", total_chunks);

            for (int test_set = 0; test_set < NUM_TEST_SETS; test_set++) {
                uint16_t collective_id = global_coll_id++;
                if (global_coll_id >= 0xFFFF) global_coll_id = 0x1000; // Wrap, skip reserved 0xFFFF

                // Generate deterministic packet order for this test run
                int level0_packet_order[MAX_CHUNKS_PER_LEVEL];
                uint32_t schedule_seed = 0xBADC0DEu ^ (uint32_t)test_set
                                       ^ ((uint32_t)NODE_RANK << 16)
                                       ^ ((uint32_t)chunk_idx << 8)
                                       ^ ((uint32_t)fmt_idx << 24);
                generate_spread_limited_order(level0_packet_order, total_chunks, schedule_seed);

                // Prepare deterministic Level 0 input data for this node
                for (int chunk = 0; chunk < (int)total_chunks; ++chunk) {
                    fill_node_chunk_data(TEST_NODE_RANK, chunk, fp_format, num_elements,
                                         input_elements_f[0][chunk], input_elements[0][chunk]);
                }

                // Simulate tree reduction to compute expected Level 4 output
                for (int chunk = 0; chunk < (int)total_chunks; ++chunk) {
                    for (int node = 0; node < NUM_NODES; ++node) {
                        fill_node_chunk_data(node, chunk, fp_format, num_elements,
                                             all_nodes_data[node], temp_node_words);
                    }
                    for (int level = 1; level <= MAX_RECURSION_LEVEL; ++level) {
                        int distance = 1 << (level - 1);
                        for (int node = 0; node < NUM_NODES; ++node) {
                            int partner = node ^ distance;
                            for (int i = 0; i < num_elements; ++i) {
                                float sum = all_nodes_data[node][i] + all_nodes_data[partner][i];
                                if (fp_format != FP_FORMAT_FP32)
                                    sum = rt_roundtrip(sum, fp_format);
                                next_level_data[node][i] = sum;
                            }
                        }
                        for (int node = 0; node < NUM_NODES; ++node) {
                            for (int i = 0; i < num_elements; ++i)
                                all_nodes_data[node][i] = next_level_data[node][i];
                        }
                    }
                    for (int i = 0; i < num_elements; ++i)
                        expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i] = all_nodes_data[TEST_NODE_RANK][i];

                    if (fp_format == FP_FORMAT_FP32) {
                        memcpy(expected_outputs[MAX_RECURSION_LEVEL][chunk],
                               expected_outputs_f[MAX_RECURSION_LEVEL][chunk],
                               DATA_PAYLOAD_LEN);
                    } else {
                        for (int i = 0; i < num_elements; i += 2) {
                            uint16_t v0 = rt_to_raw16(expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i],     fp_format);
                            uint16_t v1 = rt_to_raw16(expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i + 1], fp_format);
                            expected_outputs[MAX_RECURSION_LEVEL][chunk][i / 2] = ((uint32_t)v1 << 16) | (uint32_t)v0;
                        }
                    }
                }

                // --- Cycle measurement state ---
                uint64_t first_send_cycle = 0, last_recv_cycle = 0;
                uint64_t recv_timestamps[MAX_CHUNKS_PER_LEVEL];
                int total_packets_to_send  = (int)total_chunks;
                int total_expected_responses = (int)total_chunks;
                int responses_received = 0;
                int packets_sent = 0;
                int received_chunks_level4[MAX_CHUNKS_PER_LEVEL];
                for (int c = 0; c < MAX_CHUNKS_PER_LEVEL; c++) received_chunks_level4[c] = 0;

                const uint64_t STALL_TIMEOUT_CYCLES = 5000000000ULL;
                uint64_t last_progress_cycle = rdcycle();

                // Flush stale NIC completions
                while (nic_recv_comp_avail() > 0) {
                    reg_read16(SIMPLENIC_RECV_COMP);
                    asm volatile ("fence");
                }

                // Initialize ring buffers only ONCE, before the very first test of the entire sweep
                if (fmt_idx == 0 && chunk_idx == 0 && test_set == 0) {
                    for (int i = 0; i < NUM_RX_BUFFERS; i++) memset(rx_buffers[i], 0, BUF_SIZE);
                    rx_tail = 0;
                    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
                        while (nic_recv_req_avail() == 0);
                        reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buffers[i]);
                    }
                }

                // Synchronized start: Node 0 waits 1M cycles on the very first test set only
                if (TEST_NODE_RANK == 0 && test_set == 0 && chunk_idx == 0 && fmt_idx == 0) {
                    uint64_t wait_start = rdcycle();
                    while (rdcycle() - wait_start < 1000000) { /* spin */ }
                }

                // --- Main polling loop ---
                while (responses_received < total_expected_responses) {

                    // === SEND ===
                    if (nic_send_req_avail() > 0 && packets_sent < total_packets_to_send) {
                        int chunk = level0_packet_order[packets_sent];
                        int level = 0;

                        memset(tx_buf, 0, BUF_SIZE);
                        tx_buf[0] = 0x00; tx_buf[1] = 0x00;
                        WRITE_MAC_TO_BUF(tx_buf, 2, accel_mac);
                        WRITE_MAC_TO_BUF(tx_buf, 8, tester_mac);
                        tx_buf[14] = 0x00; tx_buf[15] = 0x00;

                        uint8_t *metadata = tx_buf + ETH_HEADER_LEN;
                        metadata[0] = (uint8_t)(collective_id & 0xFF);
                        metadata[1] = (uint8_t)((collective_id >> 8) & 0xFF);
                        metadata[2] = META_COLL_TYPE;
                        metadata[3] = META_OP;
                        metadata[4] = (uint8_t)fp_format;
                        metadata[5] = 0;
                        metadata[6] = MAX_RECURSION_LEVEL;
                        metadata[7] = (uint8_t)level;
                        uint32_t chunk_index = (uint32_t)chunk;
                        memcpy(metadata + 8,  &chunk_index,   sizeof(uint32_t));
                        memcpy(metadata + 12, &total_chunks,  sizeof(uint32_t));
                        memcpy(tx_buf + ETH_HEADER_LEN + METADATA_LEN, input_elements[0][chunk], DATA_PAYLOAD_LEN);

                        if (TEST_NODE_RANK == 0 && packets_sent == 0)
                            first_send_cycle = rdcycle();

                        nic_send(tx_buf, (unsigned long)TOTAL_PACKET_LEN);
                        packets_sent++;
                        last_progress_cycle = rdcycle();
                    }

                    // === RECEIVE ===
                    if (nic_recv_comp_avail() > 0) {
                        uint64_t rx_cycles = rdcycle();
                        rx_buf = rx_buffers[rx_tail];
                        rx_tail = (rx_tail + 1) % NUM_RX_BUFFERS;
                        int received_len = reg_read16(SIMPLENIC_RECV_COMP);
                        asm volatile ("fence");
                        memcpy(rx_local_copy, rx_buf, BUF_SIZE);

                        uint8_t *rx_payload = rx_local_copy + ETH_HEADER_LEN;
                        uint8_t response_level = rx_payload[7];
                        uint32_t response_chunk_index, response_total_chunks;
                        memcpy(&response_chunk_index,  rx_payload + 8,  sizeof(uint32_t));
                        memcpy(&response_total_chunks, rx_payload + 12, sizeof(uint32_t));

                        if (response_level != (MAX_RECURSION_LEVEL + 1)) {
                            // Not Level 4 — discard and re-post
                            memset(rx_buf, 0, BUF_SIZE);
                            while (nic_recv_req_avail() == 0);
                            reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
                            last_progress_cycle = rdcycle();
                            continue;
                        }

                        if (response_chunk_index >= MAX_CHUNKS_PER_LEVEL) {
                            printf("ERROR: Invalid chunk index %u\n", response_chunk_index);
                            sim_fail(301 + test_set);
                        }
                        if (received_chunks_level4[response_chunk_index]) {
                            printf("ERROR: Duplicate Level 4 response for chunk %u\n", response_chunk_index);
                            sim_fail(300 + test_set);
                        }
                        received_chunks_level4[response_chunk_index] = 1;

                        if (TEST_NODE_RANK == 0)
                            recv_timestamps[responses_received] = rx_cycles;

                        if (received_len != TOTAL_PACKET_LEN) {
                            printf("ERROR: Bad packet length: %d (expected %d)\n", received_len, TOTAL_PACKET_LEN);
                            sim_fail(600 + test_set);
                        }

                        // Verify metadata
                        memset(expected_rx_buf, 0, BUF_SIZE);
                        uint8_t *exp_meta = expected_rx_buf;
                        exp_meta[0] = (uint8_t)(collective_id & 0xFF);
                        exp_meta[1] = (uint8_t)((collective_id >> 8) & 0xFF);
                        exp_meta[2] = META_COLL_TYPE;
                        exp_meta[3] = META_OP;
                        exp_meta[4] = (uint8_t)fp_format;
                        exp_meta[5] = 0;
                        exp_meta[6] = MAX_RECURSION_LEVEL;
                        exp_meta[7] = MAX_RECURSION_LEVEL + 1;
                        memcpy(exp_meta + 8,  &response_chunk_index,  sizeof(uint32_t));
                        memcpy(exp_meta + 12, &response_total_chunks, sizeof(uint32_t));
                        memcpy(expected_rx_buf + METADATA_LEN,
                               expected_outputs[MAX_RECURSION_LEVEL][response_chunk_index], DATA_PAYLOAD_LEN);

                        if (memcmp(rx_payload, expected_rx_buf, METADATA_LEN) != 0) {
                            printf("ERROR: Metadata mismatch for chunk %u (fmt=%s, chunks=%u, set=%d)\n",
                                   response_chunk_index, FP_FORMAT_NAMES[fmt_idx], total_chunks, test_set + 1);
                            sim_fail(400 + test_set);
                        }

                        // ULP payload verification
                        {
                            const uint32_t* exp_w = (const uint32_t*)(expected_rx_buf + METADATA_LEN);
                            const uint32_t* got_w = (const uint32_t*)(rx_payload + METADATA_LEN);
                            int num_words = DATA_PAYLOAD_LEN / 4;
                            int bad_word = -1;
                            uint32_t bad_exp = 0, bad_got = 0, bad_diff = 0;

                            for (int w = 0; w < num_words; ++w) {
                                uint32_t a = exp_w[w], b = got_w[w];
                                if (a == b) continue;
                                if (fp_format == FP_FORMAT_FP32) {
                                    int32_t ai, bi;
                                    memcpy(&ai, &a, 4); memcpy(&bi, &b, 4);
                                    if (ai < 0) ai = 0x80000000 - ai;
                                    if (bi < 0) bi = 0x80000000 - bi;
                                    uint32_t d = (ai > bi) ? (uint32_t)(ai - bi) : (uint32_t)(bi - ai);
                                    if (d > 3u) { bad_word = w; bad_exp = a; bad_got = b; bad_diff = d; break; }
                                } else {
                                    uint16_t a_lo = a & 0xFFFF, a_hi = (a >> 16) & 0xFFFF;
                                    uint16_t b_lo = b & 0xFFFF, b_hi = (b >> 16) & 0xFFFF;
                                    if (a_lo != b_lo) {
                                        int16_t ai = (int16_t)a_lo, bi = (int16_t)b_lo;
                                        uint16_t d = (ai > bi) ? (uint16_t)(ai - bi) : (uint16_t)(bi - ai);
                                        if (d > 2u) { bad_word = w; bad_exp = a; bad_got = b; bad_diff = d; break; }
                                    }
                                    if (a_hi != b_hi) {
                                        int16_t ai = (int16_t)a_hi, bi = (int16_t)b_hi;
                                        uint16_t d = (ai > bi) ? (uint16_t)(ai - bi) : (uint16_t)(bi - ai);
                                        if (d > 2u) { bad_word = w; bad_exp = a; bad_got = b; bad_diff = d; break; }
                                    }
                                }
                            }
                            if (bad_word >= 0) {
                                printf("ERROR: Payload mismatch chunk %u (fmt=%s): word %d exp=0x%08x got=0x%08x diff=%u\n",
                                       response_chunk_index, FP_FORMAT_NAMES[fmt_idx],
                                       bad_word, bad_exp, bad_got, bad_diff);
                                sim_fail(401 + test_set);
                            }
                        }

                        responses_received++;
                        last_progress_cycle = rdcycle();
                        memset(rx_buf, 0, BUF_SIZE);
                        while (nic_recv_req_avail() == 0);
                        reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf);
                    }

                    // === STALL CHECK ===
                    if (packets_sent == total_packets_to_send && responses_received < total_expected_responses) {
                        if ((rdcycle() - last_progress_cycle) > STALL_TIMEOUT_CYCLES) {
                            printf("ERROR: Stall! fmt=%s chunks=%u set=%d sent=%d recv=%d\n",
                                   FP_FORMAT_NAMES[fmt_idx], total_chunks,
                                   test_set + 1, packets_sent, responses_received);
                            sim_fail(500 + test_set);
                        }
                    }
                } // End main polling loop

                // --- Print one CSV summary line per test set (Node 0 only) ---
                if (TEST_NODE_RANK == 0) {
                    last_recv_cycle = recv_timestamps[0];
                    for (int i = 1; i < total_expected_responses; i++)
                        if (recv_timestamps[i] > last_recv_cycle) last_recv_cycle = recv_timestamps[i];
                    uint64_t total_cycles = last_recv_cycle - first_send_cycle;
                    uint64_t total_bytes  = (uint64_t)total_chunks * DATA_PAYLOAD_LEN;
                    // CSV: format, chunk_count, test_set, cycles, throughput_MBps
                    printf("THROUGHPUT_LOG: %s, %u, %d, %lu, %.3f\n",
                           FP_FORMAT_NAMES[fmt_idx],
                           total_chunks,
                           test_set + 1,
                           (unsigned long)total_cycles,
                           (double)total_bytes / ((double)total_cycles / 1e9) / 1e6);
                }

            } // test_set loop
        } // chunk_count loop
    } // fp_format loop

    printf("\n--- Full Throughput Sweep Completed Successfully ---\n");
    sim_pass();
    return 0;
}


