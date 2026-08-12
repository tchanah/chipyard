########################################################################################################################
# file:  CMakeLists.txt
#
# usage: 
#   Edit "VARIABLES"-section to suit project requirements.
#   Build instructions:
#     cmake -S ./ -B ./build/ -D CMAKE_BUILD_TYPE=Debug
#     cmake --build ./build/ --target all
#   Cleaning:
#     cmake --build ./build/ --target clean
########################################################################################################################
cmake_minimum_required(VERSION 3.10)

project(chipyard-tests LANGUAGES C CXX)


#################################
# RISCV Toolchain
#################################

set(CMAKE_SYSTEM_NAME         "Generic" CACHE STRING "")
set(CMAKE_SYSTEM_PROCESSOR    "riscv"   CACHE STRING "")

set(TOOLCHAIN_PREFIX    "riscv64-unknown-elf-")

set(CMAKE_AR            "${TOOLCHAIN_PREFIX}ar")
set(CMAKE_ASM_COMPILER  "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_C_COMPILER    "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_LINKER        "${TOOLCHAIN_PREFIX}ld")
set(CMAKE_OBJCOPY       "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP       "${TOOLCHAIN_PREFIX}objdump")
set(CMAKE_SIZE          "${TOOLCHAIN_PREFIX}size")

set(CMAKE_EXECUTABLE_SUFFIX            ".riscv")


#################################
# Flags
#################################

# CPU architecture
set(ARCH                "rv64imafd")
set(ABI                 "lp64d")
set(CMODEL              "medany")
set(ARCH_FLAGS          -march=${ARCH} -mabi=${ABI} -mcmodel=${CMODEL})

# spec
set(SPECS               "htif_nano.specs")
set(SPEC_FLAGS          -specs=${SPECS})

# linker script
set(LINKER_SCRIPT       "htif.ld")

add_compile_options(-std=gnu99)
add_compile_options(-O2 -Wall -Wextra)
add_compile_options(-fno-common -fno-builtin-printf)
add_compile_options(${ARCH_FLAGS})
add_compile_options(${SPEC_FLAGS})

add_link_options(-static)
add_link_options(${SPEC_FLAGS})
add_link_options(-T ${LINKER_SCRIPT})


#################################
# Build
#################################

add_executable(pwm pwm.c)
add_executable(blkdev blkdev.c)
add_executable(accum accum.c)
add_executable(charcount charcount.c)
add_executable(cpp-hello cpp-hello.cpp)
add_executable(nic-loopback nic-loopback.c)
add_executable(big-blkdev big-blkdev.c)
add_executable(pingd pingd.c)
add_executable(streaming-passthrough streaming-passthrough.c)
add_executable(streaming-fir streaming-fir.c)
add_executable(nvdla nvdla.c)
add_executable(spiflashread spiflashread.c)
add_executable(spiflashwrite spiflashwrite.c)
add_executable(fft fft.c)
add_executable(gcd gcd.c)
add_executable(hello hello.c)
add_executable(mt-hello mt-hello.c)
add_executable(symmetric symmetric.c)
add_executable(ctc-test ctc-test.c)
add_executable(multi-ctc-test multi-ctc-test.c)
add_executable(triple-chiplet triple-chiplet.c)

#################################
# Disassembly
#################################

# Add a target to generate all disassemblies
add_custom_target(dump ALL)

# Function to add disassembly target for an executable
function(add_dump_target target_name)
    add_custom_target(${target_name}-dump
        BYPRODUCTS ${CMAKE_BINARY_DIR}/${target_name}.dump
        COMMAND ${CMAKE_OBJDUMP} -D $<TARGET_FILE:${target_name}> > ${CMAKE_BINARY_DIR}/${target_name}.dump
        DEPENDS ${target_name}
        COMMENT "Generating disassembly for ${target_name}"
    )
    add_dependencies(${target_name}-dump ${target_name})
    add_dependencies(dump ${target_name}-dump)
endfunction()

add_dump_target(pwm)
add_dump_target(blkdev)
add_dump_target(accum)
add_dump_target(charcount)
add_dump_target(cpp-hello)
add_dump_target(nic-loopback)
add_dump_target(big-blkdev)
add_dump_target(pingd)
add_dump_target(streaming-passthrough)
add_dump_target(streaming-fir)
add_dump_target(nvdla)
add_dump_target(spiflashread)
add_dump_target(spiflashwrite)
add_dump_target(fft)
add_dump_target(gcd)
add_dump_target(hello)
add_dump_target(mt-hello)
add_dump_target(symmetric)
add_dump_target(ctc-test)
add_dump_target(multi-ctc-test)
add_dump_target(triple-chiplet)


# Add custom command to generate spiflash.img from spiflash.py
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/spiflash.img
    COMMAND python3 ${CMAKE_SOURCE_DIR}/spiflash.py --outfile ${CMAKE_SOURCE_DIR}/spiflash.img
    DEPENDS ${CMAKE_SOURCE_DIR}/spiflash.py
    COMMENT "Generating spiflash.img"
)

# Add a target for spiflash.img
add_custom_target(spiflash_img ALL
    DEPENDS ${CMAKE_BINARY_DIR}/spiflash.img
)


#################################
# Collective (icenet/collective) tests
#
# Ported from tests/Makefile + tests/collective/{latency_tester,throughput_tester,llm-driven}/Makefile
# (fork's collective-llm-v2026.06 branch). All collective binaries need
# -Wl,-u,_printf_float -- without it, %f prints garbage because the linker
# doesn't pull in the float-formatting stub unless forced.
#################################

function(add_collective_test target src)
    add_executable(${target} ${src})
    target_link_options(${target} PRIVATE -Wl,-u,_printf_float)
    add_dump_target(${target})
endfunction()

function(add_collective_node_tests prefix src)
    foreach(rank RANGE 0 7)
        set(tgt ${prefix}-node${rank})
        add_executable(${tgt} ${src})
        target_compile_definitions(${tgt} PRIVATE NODE_RANK=${rank})
        target_link_options(${tgt} PRIVATE -Wl,-u,_printf_float)
        add_dump_target(${tgt})
    endforeach()
endfunction()

# --- Single-instance (Verilator-runnable) ---
add_collective_test(packetmodifier                collective/packetmodifier.c)
add_collective_test(recursivedoubling              collective/recursivedoubling.c)
add_collective_test(simpledmacontrollertest        collective/simpledmacontrollertest.c)
add_collective_test(recursivedoublingdma           collective/recursivedoublingdma.c)
add_collective_test(recursivedoublingwithdmafp     collective/recursivedoublingwithdmafp.c)
add_collective_test(recursivedoublingforchunks     collective/recursivedoublingforchunks.c)
add_collective_test(recursivedoublingforchunksveri collective/recursivedoublingforchunksveri.c)

# --- 8-node variants (FireSim-only; build-only under Verilator) ---
add_collective_node_tests(rdchunks         collective/recursivedoublingforchunks.c)
add_collective_node_tests(pytorch-listener collective/pytorch_listener.c)
add_collective_node_tests(rdlatency        collective/latency_tester/recursivedoubling_latency.c)
add_collective_node_tests(rdthroughput     collective/throughput_tester/recursivedoubling_throughput.c)

# --- LLM-driven tester: sweep knobs as real CMake cache vars (fixes the old
#     Makefile's "changing -D doesn't trigger a rebuild" footgun) ---
set(LLM_CHUNKS 4                     CACHE STRING "MAX_CHUNKS_PER_LEVEL (must stay < hardware maxChunks)")
set(LLM_SETS   2                     CACHE STRING "NUM_TEST_SETS")
set(LLM_LAG    2                     CACHE STRING "arrival pipeline depth in chunks (0=chunk-major, >=CHUNKS=level-major)")
set(LLM_JITTER 0                     CACHE STRING "+/- reorder window around each pipeline slot (0 = clean pipeline)")
set(LLM_SEED   1                     CACHE STRING "jitter pattern seed")
set(LLM_FORMAT FP_FORMAT_DLFLOAT     CACHE STRING "FP_FORMAT_FP32 | FP_FORMAT_BFLOAT16 | FP_FORMAT_DLFLOAT")

add_executable(recursivedoubling_llm collective/llm-driven/recursivedoubling_llm.c)
target_compile_definitions(recursivedoubling_llm PRIVATE
    MAX_CHUNKS_PER_LEVEL=${LLM_CHUNKS}
    NUM_TEST_SETS=${LLM_SETS}
    LLM_LAG=${LLM_LAG}
    LLM_JITTER=${LLM_JITTER}
    LLM_SEED=${LLM_SEED}
    TEST_FP_FORMAT=${LLM_FORMAT}
)
target_link_options(recursivedoubling_llm PRIVATE -Wl,-u,_printf_float)
add_dump_target(recursivedoubling_llm)
