#################################
# RISCV Toolchain
#################################

TARGET = riscv64-unknown-elf

GCC = $(TARGET)-gcc
CXX = $(TARGET)-g++
CP = $(TARGET)-objcopy
OBJDUMP = $(TARGET)-objdump
DG = $(TARGET)-gdb
SIZE = $(TARGET)-size

#################################
# Flags
#################################

# SoC Settings
ARCH = rv64imafdc
ABI = lp64d
ARCHFLAGS = -march=$(ARCH) -mabi=$(ABI)

CFLAGS  = -std=gnu99 -O2 -fno-common -fno-builtin-printf -Wall
CFLAGS += $(ARCHFLAGS)
LDFLAGS = -static -Wl,-u,_printf_float

# Include the main libgloss.mk relative to this directory (same depth as latency_tester/)
include ../../libgloss.mk

#################################
# Sweep knobs (override on the make command line)
#   make CHUNKS=8 SETS=1   -> -DMAX_CHUNKS_PER_LEVEL=8 -DNUM_TEST_SETS=1
# CHUNKS must stay < hardware maxChunks (config) to avoid chunk-index aliasing.
#################################

CHUNKS ?= 4
SETS   ?= 2
LAG    ?= 2      # arrival pipeline depth (chunks): 0=chunk-major, >=CHUNKS=level-major
JITTER ?= 0      # +/- reorder window around each pipeline slot (0 = clean pipeline)
SEED   ?= 1      # jitter pattern seed (fix it to compare configs; sweep it for variance)
DEFS    = -DMAX_CHUNKS_PER_LEVEL=$(CHUNKS) -DNUM_TEST_SETS=$(SETS) \
          -DLLM_LAG=$(LAG) -DLLM_JITTER=$(JITTER) -DLLM_SEED=$(SEED)

SRC = recursivedoubling_llm.c
BIN = recursivedoubling_llm.riscv

.DEFAULT_GOAL := default

#################################
# Build
#################################

recursivedoubling_llm.o: $(SRC) ../../mmio.h ../../nic.h
	$(GCC) $(CFLAGS) $(DEFS) -c $< -o $@

%.riscv: %.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@

#################################
# Recipes
#################################

.PHONY: default
default: $(BIN)

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump
	$(if $(libgloss),rm -rf $(libgloss_builddir)/)

.PHONY: dump
dump: $(BIN:.riscv=.dump)
