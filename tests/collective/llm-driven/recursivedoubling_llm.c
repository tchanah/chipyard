#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // For size_t, NULL etc.
#include <time.h>

// --- MMIO and NIC Driver Headers ---
// NOTE: this tester lives one level deeper (tests/collective/llm-driven/), so the
// driver headers are TWO levels up, not one.
#include "../../mmio.h"
#include "../../nic.h"
// rdcycle() for cycle-accurate latency measurement (provides `#define rdcycle() read_csr(cycle)`)
#include <riscv-pk/encoding.h>

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
// NUM_ELEMENTS and BYTES_PER_ELEMENT are defined below based on TEST_FP_FORMAT
// See FP Format configuration section after META_OP_SETUP
#define ETH_HEADER_LEN 16      // Ethernet header (14 bytes + 2 bytes padding)
#define METADATA_LEN 16        // Fixed metadata size
#define DATA_PAYLOAD_LEN (NUM_ELEMENTS * BYTES_PER_ELEMENT) // 256 * 4 = 1024
#define TOTAL_PACKET_LEN (ETH_HEADER_LEN + METADATA_LEN + DATA_PAYLOAD_LEN)  // 16 + 16 + 1024 = 1056

#define MAX_RECURSION_LEVEL 3 // Max level to test (matches module config)

// --- Sweep knobs (override at build time with -D...) ---
// NUM_TEST_SETS: number of full-collective iterations. Keep small for Verilator latency runs;
//   for relative-latency we want a few repeats, not exhaustive correctness.
#ifndef NUM_TEST_SETS
#define NUM_TEST_SETS 2
#endif
// MAX_CHUNKS_PER_LEVEL: chunks actually sent per level (the data-volume knob).
//   MUST stay < hardware maxChunks (config) to avoid silent chunk-index aliasing.
#ifndef MAX_CHUNKS_PER_LEVEL
#define MAX_CHUNKS_PER_LEVEL 4
#endif

// --- Arrival-order model: pipelined LAG + optional jitter (mimics real partner arrival) ---
// Packets are sent in ascending key = chunk + level*LLM_LAG + jitter.
//   LLM_LAG    : pipeline depth in chunks. 0 = chunk-major (min memory);
//                >= MAX_CHUNKS_PER_LEVEL = level-major (worst case, max memory).
//   LLM_JITTER : +/- reorder window around each slot (0 = clean deterministic pipeline).
//   LLM_SEED   : fixes the jitter pattern. FIX it when comparing TLRAM sizes; sweep it for variance.
#ifndef LLM_LAG
#define LLM_LAG 2
#endif
#ifndef LLM_JITTER
#define LLM_JITTER 0
#endif
#ifndef LLM_SEED
#define LLM_SEED 1
#endif

// Define metadata values (example)
#define META_COLL_ID   0xABCD
#define META_COLL_TYPE 0x01
#define META_OP_SUM    0x05 // Sum all values (no averaging)
#define META_OP_AVG    0x06 // Sum and average at final level
#define META_OP        META_OP_AVG  // Use AVERAGE operation for this test
#define META_OP_SETUP  0xFE // Setup Packet to configure Node Rank
#define NUM_NODES      (1 << MAX_RECURSION_LEVEL)  // 2^maxLevel = 8 nodes for maxLevel=3

// FP Format codes (matches hardware in RecursiveDoublingWithDMA.scala)
#define FP_FORMAT_FP32     0x00  // 32-bit float: 256 elements/chunk
#define FP_FORMAT_BFLOAT16 0x01  // 16-bit bfloat: 512 elements/chunk
#define FP_FORMAT_DLFLOAT  0x02  // 16-bit DLFloat: 512 elements/chunk
#define TEST_FP_FORMAT     FP_FORMAT_DLFLOAT  // Change to test other formats

// Format-conditional element configuration
#if TEST_FP_FORMAT == FP_FORMAT_FP32
    #define NUM_ELEMENTS 256
    #define BYTES_PER_ELEMENT 4
#else
    #define NUM_ELEMENTS 512
    #define BYTES_PER_ELEMENT 2
#endif

// Number of packed uint32 words in a chunk (always 256 for 1KB payload)
#define NUM_PACKED_WORDS 256

#define DEBUG_PRINT_PACKETS 0 // Set to 1 to print full TX/RX packets, 0 to disable
#define VERIFY_MAC_ROUTING 1   // Set to 1 to verify MAC routing logic

// MAC address constants (matching hardware)
#define BASE_MAC 0x00126D000000ULL  // Base MAC: 00:12:6D:00:00:00
#define TESTER_MAC_OFFSET 0x02       // Tester/Host MAC offset (rank N -> 0x02+N)
#define ACCELERATOR_MAC_OFFSET 0x22  // Accelerator MAC offset (rank N -> 0x22+N)
#define TEST_NODE_RANK 0             // Node rank for this test (0-7 for 8 nodes)

// MAC byte order macro: Uses NETWORK ORDER (MSB first, big-endian, standard Ethernet)
// MAC 0x00126D000022 writes as bytes: 00 12 6D 00 00 22 (MSB at lowest address)
//
// To switch to HOST ORDER (LSB first, little-endian):
//   Change ((5 - _i) * 8) to (_i * 8) in the shift expression below.
#define WRITE_MAC_TO_BUF(buf, offset, mac) do { \
    for (int _i = 0; _i < 6; _i++) { \
        (buf)[(offset) + _i] = (uint8_t)(((mac) >> ((5 - _i) * 8)) & 0xFF); \
    } \
} while(0)

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

// Calculate partner MAC address: baseMac | (partnerRank + ACCELERATOR_MAC_OFFSET)
// This matches the hardware logic in RecursiveDoublingWithDMA.scala
// Partner accelerators use ACCELERATOR_MAC_OFFSET (0x22), so rank N -> MAC ending in (0x22+N)
static inline uint64_t calculate_partner_mac(uint8_t level, uint8_t my_rank) {
    uint8_t partner_rank = calculate_partner_rank(level, my_rank);
    return BASE_MAC | (uint64_t)(partner_rank + ACCELERATOR_MAC_OFFSET);
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

// ============================================================================
// BFloat16 and DLFloat Conversion Functions
// ============================================================================

// Convert float to BFloat16 (round-nearest-even per IEEE 754)
// BFloat16: 1 sign, 8 exp (bias=127), 7 mantissa
// Overflow from rounding produces Infinity (e=255, m=0) per IEEE 754
static inline uint16_t float_to_bf16(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t bits = conv.u;
    
    // Round-nearest-even: check bit 15 (guard) and bits 0-14 (round+sticky)
    uint32_t guard = (bits >> 15) & 0x1;
    uint32_t round_sticky = bits & 0x7FFF;
    uint32_t lsb = (bits >> 16) & 0x1;  // LSB of result
    
    // RNE: round up if guard=1 AND (round_sticky!=0 OR lsb=1)
    uint32_t round_up = guard && (round_sticky || lsb);
    
    uint16_t result = (uint16_t)(bits >> 16);
    if (round_up) {
        result++;  // May overflow to Inf (0x7F80) which is correct per IEEE 754
    }
    return result;
}

// Convert BFloat16 to float
static inline float bf16_to_float(uint16_t bf16) {
    union { float f; uint32_t u; } conv;
    conv.u = ((uint32_t)bf16) << 16;
    return conv.f;
}

// Convert float to DLFloat (round-nearest-up per DLFloat spec)
// DLFloat: 1 sign, 6 exp (bias=31), 9 mantissa
// Per DLFloat paper: no subnormals, only e=63 m=511 is NaN-Inf
static inline uint16_t float_to_dlfloat(float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t bits = conv.u;
    
    uint32_t sign = (bits >> 31) & 0x1;
    int32_t exp32 = ((bits >> 23) & 0xFF);  // biased exponent (bias=127)
    uint32_t mant32 = bits & 0x7FFFFF;      // 23-bit mantissa
    
    // Handle FP32 special cases
    if (exp32 == 0 && mant32 == 0) {
        // Zero -> DLFloat zero (sign ignored per DLFloat spec)
        return 0x0000;
    }
    if (exp32 == 0) {
        // FP32 subnormal -> convert to DLFloat normal or zero
        // These are very small values, typically underflow to zero
        return 0x0000;
    }
    if (exp32 == 255) {
        // FP32 Inf/NaN -> DLFloat NaN-Inf (e=63, m=511)
        return 0x7FFF;  // NaN-Inf (sign ignored per spec)
    }
    
    // Convert exponent: FP32 bias=127, DLFloat bias=31
    int32_t exp_unbiased = exp32 - 127;
    int32_t exp_dlf = exp_unbiased + 31;
    
    if (exp_dlf <= 0) {
        // Underflow -> zero (no subnormals in DLFloat)
        return 0x0000;
    }
    if (exp_dlf >= 63) {
        // Overflow -> max normal (e=63, m=510), NOT NaN-Inf
        return (uint16_t)((sign << 15) | 0x7FFE);  // e=63, m=510
    }
    
    // Round-nearest-up: add 1 if guard bit is set
    // Guard bit is bit 13 (14th bit from right) of mant32
    uint32_t guard = (mant32 >> 13) & 0x1;
    uint32_t mant_dlf = (mant32 >> 14) + guard;
    
    // Handle mantissa overflow from rounding
    if (mant_dlf > 0x1FF) {
        mant_dlf = 0;
        exp_dlf++;
        if (exp_dlf >= 63) {
            return (uint16_t)((sign << 15) | 0x7FFE);  // Saturate to max normal
        }
    }
    
    return (uint16_t)((sign << 15) | (exp_dlf << 9) | mant_dlf);
}

// Convert DLFloat to float
// Per DLFloat paper: e=0 m≠0 is normal (hidden bit=1), only e=63 m=511 is NaN-Inf
static inline float dlfloat_to_float(uint16_t dlf) {
    // DLFloat NaN-Inf (sign ignored per spec)
    if (dlf == 0x7FFF || dlf == 0xFFFF) {
        union { float f; uint32_t u; } conv;
        conv.u = 0x7F800000;  // +Inf
        return conv.f;
    }
    
    // DLFloat zero (sign ignored per spec)
    if (dlf == 0x0000 || dlf == 0x8000) {
        return 0.0f;
    }
    
    uint32_t sign = (dlf >> 15) & 0x1;
    uint32_t exp_dlf = (dlf >> 9) & 0x3F;   // 6-bit exponent
    uint32_t mant_dlf = dlf & 0x1FF;        // 9-bit mantissa
    
    // DLFloat: e=0 with m≠0 is a normal number (hidden bit = 1)
    // Value = (-1)^s * 2^(e-31) * 1.m  where e can be 0
    // For e=0: exponent is 0-31 = -31
    int32_t exp_unbiased = exp_dlf - 31;
    int32_t exp32 = exp_unbiased + 127;
    
    if (exp32 <= 0) {
        // Underflow to FP32 subnormal or zero - treat as zero for simplicity
        return sign ? -0.0f : 0.0f;
    }
    if (exp32 >= 255) {
        // Overflow to FP32 Inf
        union { float f; uint32_t u; } conv;
        conv.u = (sign << 31) | 0x7F800000;
        return conv.f;
    }
    
    // Extend mantissa from 9 bits to 23 bits
    uint32_t mant32 = mant_dlf << 14;
    
    union { float f; uint32_t u; } conv;
    conv.u = (sign << 31) | (exp32 << 23) | mant32;
    return conv.f;
}

// ============================================================================
// Format-Aware Conversion Wrappers (selected by TEST_FP_FORMAT)
// ============================================================================

// Convert float to the 16-bit format and back, capturing precision loss
// Returns the float value after round-trip through the 16-bit format
static inline float float_roundtrip_16bit(float f) {
#if TEST_FP_FORMAT == FP_FORMAT_BFLOAT16
    return bf16_to_float(float_to_bf16(f));
#elif TEST_FP_FORMAT == FP_FORMAT_DLFLOAT
    return dlfloat_to_float(float_to_dlfloat(f));
#else
    return f;  // FP32: no precision loss
#endif
}

// Convert float to raw 16-bit representation (for buffer packing)
static inline uint16_t float_to_raw16(float f) {
#if TEST_FP_FORMAT == FP_FORMAT_BFLOAT16
    return float_to_bf16(f);
#elif TEST_FP_FORMAT == FP_FORMAT_DLFLOAT
    return float_to_dlfloat(f);
#else
    return 0;  // Should not be called for FP32
#endif
}

// Convert raw 16-bit representation to float (for verification)
static inline float raw16_to_float(uint16_t raw) {
#if TEST_FP_FORMAT == FP_FORMAT_BFLOAT16
    return bf16_to_float(raw);
#elif TEST_FP_FORMAT == FP_FORMAT_DLFLOAT
    return dlfloat_to_float(raw);
#else
    return 0.0f;  // Should not be called for FP32
#endif
}

// Generate the nth permutation of numbers 0 to n-1 (0-indexed)
// Self-contained LCG for arrival-order jitter — kept separate from the global rand()
// (which is reserved for reproducible input data, srand(1234)).
static uint32_t llm_rng_state = 1;
static inline uint32_t llm_rand(void) {
    llm_rng_state = llm_rng_state * 1103515245u + 12345u;
    return (llm_rng_state >> 16) & 0x7FFFu;
}

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

// Helper function to print float elements
void print_elements_f(const char* title, const uint32_t* elements, size_t num_elements) {
    // Cast size_t to unsigned long and use %lu
    printf("%s (%lu elements, %lu bytes total):\n", title, (unsigned long)num_elements, (unsigned long)(num_elements * sizeof(uint32_t)));
    
#if TEST_FP_FORMAT == FP_FORMAT_FP32
    // FP32: each uint32 is one float
    for (size_t i = 0; i < num_elements; ++i) {
        float val;
        memcpy(&val, &elements[i], sizeof(float));
        printf("%12.4f ", val); // Print as float
        if ((i + 1) % 8 == 0 || i == num_elements - 1) {
            printf("\n");
        }
    }
#else
    // 16-bit formats: each uint32 contains 2 packed 16-bit values
    // Unpack and convert using raw16_to_float
    size_t element_count = 0;
    for (size_t i = 0; i < num_elements; ++i) {
        uint16_t low = (uint16_t)(elements[i] & 0xFFFF);
        uint16_t high = (uint16_t)((elements[i] >> 16) & 0xFFFF);
        printf("%12.4f ", raw16_to_float(low));
        element_count++;
        if (element_count % 8 == 0) printf("\n");
        printf("%12.4f ", raw16_to_float(high));
        element_count++;
        if (element_count % 8 == 0) printf("\n");
    }
    if (element_count % 8 != 0) printf("\n");
#endif
    printf("\n");
}

// --- Helper: Send Setup Packet to configure accelerator rank ---
// Sends Setup packet (opCode=0xFE) with collID=0xFFFF to request ACK.
// Returns 1 on success (ACK received), 0 on failure (timeout).
int send_setup_packet(int rank, uint64_t tester_mac, uint64_t accel_mac) {
    printf("Sending Setup Packet to accelerator (Setting Rank=%d)...\n", rank);
    
    // Buffers
    static uint8_t tx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t rx_buf_setup[BUF_SIZE] __attribute__((aligned(64)));
    
    memset(tx_buf_setup, 0, TOTAL_PACKET_LEN);
    
    // Ethernet header: [padding(2)] [dst(6)] [src(6)] [ethtype(2)]
    tx_buf_setup[0] = 0x00; tx_buf_setup[1] = 0x00;  // Padding
    WRITE_MAC_TO_BUF(tx_buf_setup, 2, accel_mac);    // Dst = accelerator
    WRITE_MAC_TO_BUF(tx_buf_setup, 8, tester_mac);   // Src = tester
    tx_buf_setup[14] = 0x00; tx_buf_setup[15] = 0x00;  // EtherType
    
    // Metadata
    uint8_t *meta = tx_buf_setup + ETH_HEADER_LEN;
    meta[0] = 0xFF; meta[1] = 0xFF;  // CollID = 0xFFFF (triggers ACK)
    meta[2] = 0;                      // Type
    meta[3] = META_OP_SETUP;          // Operation: SETUP (0xFE)
    meta[5] = (uint8_t)rank;          // New Rank to configure (Setup packets only)
    
    // Pre-post receive buffer for ACK
    reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
    
    // Send Setup packet
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
            
            printf("ACK Packet: collID=0x%04x, opCode=0x%02x, level=%u\n",
                    coll_id, op_code, response_level);
            
            // Check for Setup ACK: collID=0xFFFF, opCode=0xFE, level=1
            if (coll_id == 0xFFFF && op_code == META_OP_SETUP && response_level == 1) {
                printf("Setup ACK received! Rank=%d confirmed.\n", rank);
                return 1;  // Success
            } else {
                printf("Ignoring non-ACK packet, continuing wait...\n");
                // Re-post buffer and continue waiting
                reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)rx_buf_setup);
            }
        }
        timeout--;
    }
    
    printf("ERROR: Setup ACK timeout! Continuing anyway...\n");
    return 0;  // Failure
}

// --- Main Test ---

int main() {
    // Suppress unused function warnings (async recv; fixed packet order so no shuffle).
    (void)nic_recv;
    (void)generate_nth_permutation;

    printf("Starting RecursiveDoubling Bare-Metal Test with SimpleNIC...\n");
    srand(1234);
    fflush(stdout);
    printf("Running %d test sets with all possible packet orderings\n", NUM_TEST_SETS);
    printf("Each set: %d levels, %d elements (%d bytes payload per chunk)\n",
           NUM_LEVELS, NUM_ELEMENTS, DATA_PAYLOAD_LEN);
    printf("Max Recursion Level: %d\n", MAX_RECURSION_LEVEL);
    printf("Operation Mode: %s (0x%02X), Nodes: %d\n", 
           (META_OP == META_OP_AVG) ? "AVERAGE" : "SUM", META_OP, NUM_NODES);
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

    // Buffers to hold the chunked input data payloads
    // Float arrays: NUM_ELEMENTS floats for calculation
    // Uint32 arrays: NUM_PACKED_WORDS (256) for transmission (packed 16-bit or direct 32-bit)
    static float input_elements_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static float expected_outputs_f[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_ELEMENTS] __attribute__((aligned(64)));
    static uint32_t input_elements[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_PACKED_WORDS] __attribute__((aligned(64)));
    static uint32_t expected_outputs[NUM_LEVELS][MAX_CHUNKS_PER_LEVEL][NUM_PACKED_WORDS] __attribute__((aligned(64)));
    static int packet_order[NUM_LEVELS * MAX_CHUNKS_PER_LEVEL];  // Order for all chunk packets
    static int packet_key[NUM_LEVELS * MAX_CHUNKS_PER_LEVEL];    // sort keys for the arrival-order model

    // --- 1. NIC Initialization (Implicit) ---
    printf("SimpleNIC assumed ready after reset.\n");
    uint64_t mac = nic_macaddr();
    printf("NIC MAC Address: %012lx\n", (unsigned long)mac);
    
    // Compute MAC addresses for this node
    uint64_t tester_mac = BASE_MAC | ((uint64_t)(TEST_NODE_RANK + TESTER_MAC_OFFSET));
    uint64_t accel_mac  = BASE_MAC | ((uint64_t)(TEST_NODE_RANK + ACCELERATOR_MAC_OFFSET));
    
    printf("Node Rank: %d\n", TEST_NODE_RANK);
    print_mac("Tester MAC", tester_mac);
    print_mac("Accelerator MAC", accel_mac);
    
    // Set NIC MAC address to tester MAC
    reg_write64(SIMPLENIC_MACADDR, tester_mac);
    asm volatile ("fence");
    
    #if VERIFY_MAC_ROUTING
    printf("\n--- MAC Routing Verification Setup ---\n");
    printf("Expected Partner MACs for each level:\n");
    for (uint8_t level = 1; level <= MAX_RECURSION_LEVEL; level++) {
        uint64_t partner_mac = calculate_partner_mac(level, TEST_NODE_RANK);
        uint8_t partner_rank = calculate_partner_rank(level, TEST_NODE_RANK);
        printf("  Level %d: Partner rank=%d, ", level, partner_rank);
        print_mac("Partner MAC", partner_mac);
    }
    printf("  Level %d (final): Should return to Tester MAC (0x%012lx)\n", 
           MAX_RECURSION_LEVEL + 1, (unsigned long)tester_mac);
    printf("\nNote: Hardware debug prints in NIC.scala will show actual MACs used.\n\n");
    #endif

    // --- Send Setup Packet to configure accelerator rank ---
    printf("=== Sending Setup Packet ===\n");
    if (!send_setup_packet(TEST_NODE_RANK, tester_mac, accel_mac)) {
        printf("WARNING: Setup packet failed, but continuing with test...\n");
    } else {
        printf("Setup completed successfully.\n");
    }


    // --- 2. Run Multiple Test Sets ---
    for (int test_set = 0; test_set < NUM_TEST_SETS; test_set++) {
        printf("\n=== Starting Test Set %d ===\n", test_set + 1);

        // Generate unique collective ID for this test set
        uint16_t test_collective_id = (uint16_t)(0x1000 + test_set); // Sequential ID starting from 0x1000
        printf("Collective ID for this set: 0x%04X\n", test_collective_id);

        // For chunked testing, we'll test with a specific number of chunks
        uint32_t total_chunks = MAX_CHUNKS_PER_LEVEL;
        printf("Testing with %u chunks per level\n", total_chunks);
        
        // Build the send order from the pipelined arrival model (LAG + optional jitter).
        // Packet i encodes level=i/total_chunks, chunk=i%total_chunks.
        // key = chunk + level*LLM_LAG + jitter ; lower key = sent earlier. This interleaves
        // higher-level packets into the level-0 stream so chunks finish and free memory
        // continuously (LAG=0 chunk-major .. LAG>=total_chunks level-major). Deterministic per
        // (SEED,test_set) so runs stay comparable; jitter only wobbles within +/-LLM_JITTER.
        int total_packets = NUM_LEVELS * total_chunks;
        llm_rng_state = (uint32_t)(LLM_SEED + test_set + 1);
        for (int i = 0; i < total_packets; i++) {
            int level  = i / total_chunks;
            int chunk  = i % total_chunks;
            int jitter = (LLM_JITTER > 0)
                       ? ((int)(llm_rand() % (2u * (unsigned)LLM_JITTER + 1u)) - LLM_JITTER)
                       : 0;
            packet_key[i]   = chunk + level * LLM_LAG + jitter;
            packet_order[i] = i;
        }
        // Stable selection sort of packet_order by key (tie-break by packet index -> lower level first).
        for (int a = 0; a < total_packets - 1; a++) {
            int best = a;
            for (int b = a + 1; b < total_packets; b++) {
                int kb = packet_key[packet_order[b]], kbest = packet_key[packet_order[best]];
                if (kb < kbest || (kb == kbest && packet_order[b] < packet_order[best])) best = b;
            }
            if (best != a) { int t = packet_order[a]; packet_order[a] = packet_order[best]; packet_order[best] = t; }
        }

        printf("Total packets to send: %d (LAG=%d JITTER=%d SEED=%d)\n",
               total_packets, LLM_LAG, LLM_JITTER, LLM_SEED);
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
                    float raw_val = ((float)rand() / (float)RAND_MAX) * max_rand_val;
                    // Apply precision loss for 16-bit formats (round-trip through format)
                    input_elements_f[p][chunk][i] = float_roundtrip_16bit(raw_val);
                }
                
                // Pack input values into uint32 buffer based on format
#if TEST_FP_FORMAT == FP_FORMAT_FP32
                // FP32: direct copy of bit patterns
                memcpy(input_elements[p][chunk], input_elements_f[p][chunk], DATA_PAYLOAD_LEN);
#else
                // 16-bit formats: pack 2 elements per uint32
                for (int i = 0; i < NUM_ELEMENTS; i += 2) {
                    uint16_t val0 = float_to_raw16(input_elements_f[p][chunk][i]);
                    uint16_t val1 = float_to_raw16(input_elements_f[p][chunk][i + 1]);
                    // Pack low element in low 16 bits, high element in high 16 bits
                    input_elements[p][chunk][i / 2] = ((uint32_t)val1 << 16) | (uint32_t)val0;
                }
#endif
            }
        }

        // Pre-calculate expected outputs for each level and chunk using floating-point math
        // For 16-bit formats: model hardware behavior by round-tripping through 16-bit at each level
        for (int chunk = 0; chunk < total_chunks; ++chunk) {
            // Level 0: output equals input (already round-tripped when input was generated)
            memcpy(expected_outputs_f[0][chunk], input_elements_f[0][chunk], NUM_ELEMENTS * sizeof(float));
            
            // Higher levels: output = input + previous_level_output
            // For 16-bit formats, round-trip through format at each level to match hardware precision
            for (int level = 1; level <= MAX_RECURSION_LEVEL; level++) {
                for (int i = 0; i < NUM_ELEMENTS; i++) {
                    float sum = input_elements_f[level][chunk][i] + expected_outputs_f[level-1][chunk][i];
#if TEST_FP_FORMAT != FP_FORMAT_FP32
                    // Round-trip through 16-bit format to match hardware precision loss
                    // Hardware: read 16-bit from memory -> add 16-bit -> store 16-bit result
                    sum = float_roundtrip_16bit(sum);
#endif
                    expected_outputs_f[level][chunk][i] = sum;
                }
            }
            
            // For AVERAGE operation, divide final level output by number of nodes
            // This matches the hardware behavior when operationReg == OP_AVERAGE
            if (META_OP == META_OP_AVG) {
                for (int i = 0; i < NUM_ELEMENTS; i++) {
                    expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i] /= (float)NUM_NODES;
#if TEST_FP_FORMAT != FP_FORMAT_FP32
                    // Round-trip after division to match hardware divideByPow2
                    expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i] = float_roundtrip_16bit(expected_outputs_f[MAX_RECURSION_LEVEL][chunk][i]);
#endif
                }
            }
        }

        // After calculating all expected float values, copy their bit patterns to the uint32_t array for verification
        for (int p = 0; p < NUM_LEVELS; p++) {
            for (int chunk = 0; chunk < total_chunks; ++chunk) {
#if TEST_FP_FORMAT == FP_FORMAT_FP32
                // FP32: direct copy of bit patterns
                memcpy(expected_outputs[p][chunk], expected_outputs_f[p][chunk], DATA_PAYLOAD_LEN);
#else
                // 16-bit formats: pack 2 elements per uint32
                for (int i = 0; i < NUM_ELEMENTS; i += 2) {
                    uint16_t val0 = float_to_raw16(expected_outputs_f[p][chunk][i]);
                    uint16_t val1 = float_to_raw16(expected_outputs_f[p][chunk][i + 1]);
                    expected_outputs[p][chunk][i / 2] = ((uint32_t)val1 << 16) | (uint32_t)val0;
                }
#endif
            }
        }

        // --- Non-blocking send/receive to avoid deadlock ---
        printf("\n--- Sending packets and polling for responses ---\n");
        int total_packets_to_send = NUM_LEVELS * total_chunks;
        // Only expect Level 4 (final) responses - one per chunk
        // Intermediate responses (Level 1-3) go to switchio and are discarded
        int total_expected_responses = total_chunks;  // Only Level 4 responses
        int responses_received = 0;
        int packets_sent = 0;
        int received_chunks_l4[MAX_CHUNKS_PER_LEVEL]; // Track Level 4 chunks received

        // A stall detector
        const uint64_t STALL_TIMEOUT_CYCLES = 10000000; // Adjust as needed
        uint64_t stall_counter = 0;

        // Initialize tracking array for Level 4 chunks only
        for (int c = 0; c < MAX_CHUNKS_PER_LEVEL; c++) {
            received_chunks_l4[c] = 0;
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

        // --- Latency measurement: bracket the whole collective ---
        // cycle_start is taken right before the first packet goes out; cycle_end when the last
        // Level-4 response arrives. Cycle-accurate and deterministic in Verilator.
        uint64_t cycle_start = rdcycle();
        uint64_t cycle_end   = cycle_start;

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
                // For Level 0 packets: from Tester to Accelerator
                // For Level 1-3 packets: Simulating packet FROM partner accelerator TO our accelerator
                uint64_t src_mac, dst_mac;

                if (level == 0) {
                    // Level 0: From Tester to our Accelerator
                    src_mac = tester_mac;
                    dst_mac = accel_mac;
                } else {
                    // Level 1-3: Simulating INCOMING packet FROM Partner Accelerator TO our Accelerator
                    // With loopback harness, these packets will be looped back from switchio.out to switchio.in
                    uint8_t partner_rank = calculate_partner_rank(level, TEST_NODE_RANK);
                    src_mac = BASE_MAC | (uint64_t)(partner_rank + ACCELERATOR_MAC_OFFSET); // Source = Partner Accelerator
                    dst_mac = accel_mac; // Dest = Our Accelerator
                }
                
                // Ethernet header wire format: [padding(2B) | dstmac(6B) | srcmac(6B) | ethType(2B)]
                // Use WRITE_MAC_TO_BUF for consistent network byte order
                tx_buf[0] = 0x00;   // Padding byte 0
                tx_buf[1] = 0x00;   // Padding byte 1
                WRITE_MAC_TO_BUF(tx_buf, 2, dst_mac);   // Destination MAC (6 bytes)
                WRITE_MAC_TO_BUF(tx_buf, 8, src_mac);   // Source MAC (6 bytes)
                tx_buf[14] = 0x00;  // EtherType byte 0
                tx_buf[15] = 0x00;  // EtherType byte 1
                
                // --- Metadata (16 bytes, starting at offset 16) ---
                uint8_t *metadata = tx_buf + ETH_HEADER_LEN;
                metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                metadata[2] = META_COLL_TYPE;
                metadata[3] = META_OP;
                metadata[4] = TEST_FP_FORMAT;  // FP format
                metadata[5] = 0;  // Reserved (rank only used in Setup packets)
                metadata[6] = MAX_RECURSION_LEVEL;
                metadata[7] = (uint8_t)level;

                // --- Word 1 (Offset 8 in metadata, 24 total): Chunked Metadata ---
                uint32_t chunk_index = (uint32_t)chunk;
                memcpy(metadata + 8, &chunk_index, sizeof(uint32_t));
                memcpy(metadata + 12, &total_chunks, sizeof(uint32_t));

                // --- Data Payload (starting at offset 32) ---
                memcpy(tx_buf + ETH_HEADER_LEN + METADATA_LEN, input_elements[level][chunk], DATA_PAYLOAD_LEN);

                // Verify MAC addresses in buffer before sending (especially for first packet)
                // Read in NETWORK ORDER (MSB first) to match WRITE_MAC_TO_BUF
                uint64_t tx_src_mac_check = 0, tx_dst_mac_check = 0;
                for (int i = 0; i < 6; i++) {
                    tx_dst_mac_check |= ((uint64_t)tx_buf[2 + i]) << ((5 - i) * 8);
                    tx_src_mac_check |= ((uint64_t)tx_buf[8 + i]) << ((5 - i) * 8);
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
                // Extract MAC addresses in NETWORK ORDER (MSB first) to match WRITE_MAC_TO_BUF
                uint64_t rx_dst_mac = 0, rx_src_mac = 0;
                for (int i = 0; i < 6; i++) {
                    rx_dst_mac |= ((uint64_t)rx_buf[2 + i]) << ((5 - i) * 8);  // dstmac bytes 2-7, MSB first
                    rx_src_mac |= ((uint64_t)rx_buf[8 + i]) << ((5 - i) * 8);  // srcmac bytes 8-13, MSB first
                }
                
                // --- Strip Ethernet header from received packet ---
                // The prepender adds an Ethernet header to outgoing packets, so we need to skip it
                uint8_t *rx_payload = rx_buf + ETH_HEADER_LEN;
                
                // --- Extract response level from metadata ---
                uint8_t response_level = rx_payload[7];
                uint32_t response_chunk_index, response_total_chunks;
                memcpy(&response_chunk_index, rx_payload + 8, sizeof(uint32_t));
                memcpy(&response_total_chunks, rx_payload + 12, sizeof(uint32_t));

                #if DEBUG_PRINT_PACKETS
                    printf("Received response for level %u, chunk %u\n", response_level, response_chunk_index);
                #endif
                
                // Verify we're only receiving Level 4 (final) responses
                // Intermediate responses (Level 1-3) go to switchio and are discarded
                if (response_level != MAX_RECURSION_LEVEL + 1) {
                    printf("WARNING: Received non-final response level %u (expected %d), chunk %u\n", 
                           response_level, MAX_RECURSION_LEVEL + 1, response_chunk_index);
                }
                
                if (received_chunks_l4[response_chunk_index]) {
                    printf("ERROR: Duplicate Level 4 response for chunk %u\n", response_chunk_index);
                    sim_fail(300 + test_set);
                }
                received_chunks_l4[response_chunk_index] = 1;

                // Construct expected response using the pre-calculated expected output
                memset(expected_rx_buf, 0, BUF_SIZE);
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                uint8_t *expected_metadata = expected_rx_buf;
                expected_metadata[0] = (uint8_t)(test_collective_id & 0xFF);
                expected_metadata[1] = (uint8_t)((test_collective_id >> 8) & 0xFF);
                expected_metadata[2] = META_COLL_TYPE;
                expected_metadata[3] = META_OP;
                expected_metadata[4] = TEST_FP_FORMAT;  // FP format
                expected_metadata[5] = 0;  // Reserved
                expected_metadata[6] = MAX_RECURSION_LEVEL;
                expected_metadata[7] = response_level;

                // --- Word 1 (Offset 8): Expected Chunked Metadata ---
                memcpy(expected_metadata + 8, &response_chunk_index, sizeof(uint32_t));
                memcpy(expected_metadata + 12, &response_total_chunks, sizeof(uint32_t));

                // Copy expected chunk data - Level 4 output is sum of all 4 input levels
                // expected_outputs[MAX_RECURSION_LEVEL] = Level0 + Level1 + Level2 + Level3
                memcpy(expected_rx_buf + METADATA_LEN, expected_outputs[MAX_RECURSION_LEVEL][response_chunk_index], DATA_PAYLOAD_LEN);

                #if DEBUG_PRINT_PACKETS
                    printf("\n--- Received Packet Details ---\n");
                    print_packet_metadata("RX", rx_payload);
                    print_elements_f("RX", (const uint32_t*)(rx_payload + METADATA_LEN), 16);

                    printf("\n--- Expected Packet Details ---\n");
                    print_packet_metadata("Expected", expected_rx_buf);
                    print_elements_f("Expected", (const uint32_t*)(expected_rx_buf + METADATA_LEN), 16);
                #endif

                #if VERIFY_MAC_ROUTING
                    // Verify MAC addresses for Level 4 (final) responses
                    // In this test mode:
                    // - Intermediate packets (Level 1-3) go to switchio and are discarded
                    // - Only final packets (Level 4) come to netio (tester)
                    // - Source MAC should be our accelerator's MAC
                    // - Destination MAC should be the tester's MAC (Level 0 source)
                    uint64_t expected_src_mac = accel_mac;  // Our accelerator adds its MAC as source
                    uint64_t expected_dst_mac = tester_mac; // Level 4 always goes to tester
                    
                    // Verify source MAC is accelerator's MAC
                    if (rx_src_mac != expected_src_mac) {
                        printf("ERROR: Source MAC mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                        printf("  Expected source MAC: 0x%012lx (accelerator)\n", (unsigned long)expected_src_mac);
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

                // Verify response
                // Note: rx_payload (after stripping Ethernet header) starts with metadata
                // 1) Strictly compare metadata
                if (memcmp(rx_payload, expected_rx_buf, METADATA_LEN) != 0) {
                    printf("ERROR: Metadata mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                    sim_fail(400 + test_set);
                }

                // 2) Compare payload with ULP tolerance per 32-bit word
                // For FP32: 256 words, each is a float
                // For 16-bit: 256 words, each contains 2 packed 16-bit elements
                {
                    const uint32_t* exp_words = (const uint32_t*)(expected_rx_buf + METADATA_LEN);
                    const uint32_t* got_words = (const uint32_t*)(rx_payload + METADATA_LEN);
                    int num_words = DATA_PAYLOAD_LEN / 4;  // Always 256 for 1KB payload
                    int bad_word = -1;
                    uint32_t bad_exp = 0, bad_got = 0, bad_diff = 0;

                    for (int w = 0; w < num_words; ++w) {
                        uint32_t a = exp_words[w];
                        uint32_t b = got_words[w];
                        if (a == b) continue;

#if TEST_FP_FORMAT == FP_FORMAT_FP32
                        // FP32: ULP comparison for single float
                        int32_t ai, bi;
                        memcpy(&ai, &a, sizeof(int32_t));
                        memcpy(&bi, &b, sizeof(int32_t));
                        if (ai < 0) ai = 0x80000000 - ai;
                        if (bi < 0) bi = 0x80000000 - bi;
                        uint32_t udiff = (ai > bi) ? (uint32_t)(ai - bi) : (uint32_t)(bi - ai);

                        if (udiff > 3u) {
                            bad_word = w;
                            bad_exp = a;
                            bad_got = b;
                            bad_diff = udiff;
                            break;
                        }
#else
                        // 16-bit formats: Compare each 16-bit element in the word
                        uint16_t a_lo = a & 0xFFFF, a_hi = (a >> 16) & 0xFFFF;
                        uint16_t b_lo = b & 0xFFFF, b_hi = (b >> 16) & 0xFFFF;
                        
                        // Compare low element
                        if (a_lo != b_lo) {
                            int16_t ai = (int16_t)a_lo, bi = (int16_t)b_lo;
                            uint16_t udiff = (ai > bi) ? (uint16_t)(ai - bi) : (uint16_t)(bi - ai);
                            if (udiff > 2u) {
                                bad_word = w;
                                bad_exp = a;
                                bad_got = b;
                                bad_diff = udiff;
                                break;
                            }
                        }
                        // Compare high element
                        if (a_hi != b_hi) {
                            int16_t ai = (int16_t)a_hi, bi = (int16_t)b_hi;
                            uint16_t udiff = (ai > bi) ? (uint16_t)(ai - bi) : (uint16_t)(bi - ai);
                            if (udiff > 2u) {
                                bad_word = w;
                                bad_exp = a;
                                bad_got = b;
                                bad_diff = udiff;
                                break;
                            }
                        }
#endif
                    }

                    if (bad_word >= 0) {
                        printf("ERROR: Payload mismatch for level %u, chunk %u!\n", response_level, response_chunk_index);
                        printf("  First differing word %d: Expected 0x%08x Got 0x%08x (diff %u)\n",
                               bad_word, bad_exp, bad_got, bad_diff);
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
                    // All packets sent but responses never completed -> the pipeline is wedged.
                    // The most likely cause in this study is the DEADLOCK FLOOR: numMemoryBlocks is
                    // too small for the working set, so the allocator can never free a block to make
                    // progress. This is a DISTINCT outcome from "slow but passes".
                    printf("DEADLOCK/STALL: sent all %d packets but only %d/%d responses returned.\n",
                           packets_sent, responses_received, total_expected_responses);
                    printf("  Likely memory-starvation floor: increase numMemoryBlocks or lower MAX_CHUNKS_PER_LEVEL.\n");
                    sim_fail(500 + test_set);
                }
            }
        } // End of main polling loop
        cycle_end = rdcycle();  // last Level-4 response has arrived

        printf("All %d packets sent and %d expected responses received.\n", packets_sent, responses_received);

        // --- Cycle-accurate latency report (the headline metric) ---
        // TOTAL_CYCLES   : first-packet -> last-response, this collective.
        // CYCLES_PER_CHUNK: amortized per data chunk (steady-state proxy).
        // Greppable single-line CSV so a future sweep can parse it.
        {
            uint64_t total_cycles = cycle_end - cycle_start;
            uint64_t per_chunk    = total_cycles / (uint64_t)total_chunks;
            printf("LATENCY_CSV set=%d collId=0x%04x numChunksPerLevel=%d totalChunks=%lu "
                   "TOTAL_CYCLES=%lu CYCLES_PER_CHUNK=%lu\n",
                   test_set + 1, test_collective_id, MAX_CHUNKS_PER_LEVEL,
                   (unsigned long)total_chunks,
                   (unsigned long)total_cycles, (unsigned long)per_chunk);
        }
        printf("=== Test Set %d Completed Successfully ===\n", test_set + 1);
    }

    printf("\n--- All %d Test Sets Completed Successfully ---\n", NUM_TEST_SETS);
    sim_pass();
    return 0;
}

