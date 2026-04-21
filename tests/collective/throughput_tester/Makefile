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

# Include the main libgloss.mk relative to this directory
include ../../libgloss.mk

# The source file for this directory
SRC = recursivedoubling_throughput.c

# Per-node binaries
NODES = node0 node1 node2 node3 node4 node5 node6 node7

BINARIES = $(addsuffix .riscv, $(NODES))

.DEFAULT_GOAL := default

#################################
# Build
#################################

# Explicit rules for generating the object files with different NODE_RANK defines
node0.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=0 -c $< -o $@
node1.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=1 -c $< -o $@
node2.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=2 -c $< -o $@
node3.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=3 -c $< -o $@
node4.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=4 -c $< -o $@
node5.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=5 -c $< -o $@
node6.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=6 -c $< -o $@
node7.o: $(SRC) ../../mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=7 -c $< -o $@

# Pattern rule for linking
%.riscv: %.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@


#################################
# Recipes
#################################

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump
	$(if $(libgloss),rm -rf $(libgloss_builddir)/)

.PHONY: default
default: $(BINARIES)

.PHONY: dumps
dumps: $(addsuffix .dump, $(NODES))
