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

include libgloss.mk

PROGRAMS = pwm blkdev accum charcount nic-loopback big-blkdev pingd \
           streaming-passthrough streaming-fir nvdla spiflashread spiflashwrite fft gcd \
           hello mt-hello symmetric \
		   collective/packetmodifier collective/recursivedoubling collective/simpledmacontrollertest \
		   collective/recursivedoublingdma collective/recursivedoublingwithdmafp \
		   collective/recursivedoublingforchunks collective/recursivedoublingforchunksveri

# Per-node binaries for 8-node recursive doubling test
RECURSIVE_DOUBLING_NODES = collective/recursivedoublingforchunks-node0 \
                           collective/recursivedoublingforchunks-node1 \
                           collective/recursivedoublingforchunks-node2 \
                           collective/recursivedoublingforchunks-node3 \
                           collective/recursivedoublingforchunks-node4 \
                           collective/recursivedoublingforchunks-node5 \
                           collective/recursivedoublingforchunks-node6 \
                           collective/recursivedoublingforchunks-node7

# Per-node binaries for PyTorch Listener
PYTORCH_LISTENER_NODES = collective/pytorch_listener-node0 \
                         collective/pytorch_listener-node1 \
                         collective/pytorch_listener-node2 \
                         collective/pytorch_listener-node3 \
                         collective/pytorch_listener-node4 \
                         collective/pytorch_listener-node5 \
                         collective/pytorch_listener-node6 \
                         collective/pytorch_listener-node7

# Add per-node binaries to the main PROGRAMS list so they build by default
PROGRAMS += $(RECURSIVE_DOUBLING_NODES)
PROGRAMS += $(PYTORCH_LISTENER_NODES)


.DEFAULT_GOAL := default


#################################
# Build
#################################

spiflash.img: spiflash.py
	python3 $<

%.o: %.S
	$(GCC) $(CFLAGS) -D__ASSEMBLY__=1 -c $< -o $@

%.o: %.c mmio.h spiflash.h
	$(GCC) $(CFLAGS) -c $< -o $@

# Build per-node binaries with different NODE_RANK defines
# Pattern: node0, node1, ..., node7
collective/recursivedoublingforchunks-node0.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=0 -c $< -o $@
collective/recursivedoublingforchunks-node1.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=1 -c $< -o $@
collective/recursivedoublingforchunks-node2.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=2 -c $< -o $@
collective/recursivedoublingforchunks-node3.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=3 -c $< -o $@
collective/recursivedoublingforchunks-node4.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=4 -c $< -o $@
collective/recursivedoublingforchunks-node5.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=5 -c $< -o $@
collective/recursivedoublingforchunks-node6.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=6 -c $< -o $@
collective/recursivedoublingforchunks-node7.o: collective/recursivedoublingforchunks.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=7 -c $< -o $@

collective/recursivedoublingforchunks-node0.riscv: collective/recursivedoublingforchunks-node0.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node1.riscv: collective/recursivedoublingforchunks-node1.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node2.riscv: collective/recursivedoublingforchunks-node2.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node3.riscv: collective/recursivedoublingforchunks-node3.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node4.riscv: collective/recursivedoublingforchunks-node4.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node5.riscv: collective/recursivedoublingforchunks-node5.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node6.riscv: collective/recursivedoublingforchunks-node6.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/recursivedoublingforchunks-node7.riscv: collective/recursivedoublingforchunks-node7.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@

# Build per-node binaries for PyTorch Listener
collective/pytorch_listener-node0.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=0 -c $< -o $@
collective/pytorch_listener-node1.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=1 -c $< -o $@
collective/pytorch_listener-node2.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=2 -c $< -o $@
collective/pytorch_listener-node3.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=3 -c $< -o $@
collective/pytorch_listener-node4.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=4 -c $< -o $@
collective/pytorch_listener-node5.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=5 -c $< -o $@
collective/pytorch_listener-node6.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=6 -c $< -o $@
collective/pytorch_listener-node7.o: collective/pytorch_listener.c mmio.h
	$(GCC) $(CFLAGS) -DNODE_RANK=7 -c $< -o $@

collective/pytorch_listener-node0.riscv: collective/pytorch_listener-node0.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node1.riscv: collective/pytorch_listener-node1.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node2.riscv: collective/pytorch_listener-node2.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node3.riscv: collective/pytorch_listener-node3.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node4.riscv: collective/pytorch_listener-node4.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node5.riscv: collective/pytorch_listener-node5.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node6.riscv: collective/pytorch_listener-node6.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@
collective/pytorch_listener-node7.riscv: collective/pytorch_listener-node7.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@

%.riscv: %.o $(libgloss)
	$(GCC) $(LDFLAGS) $< -o $@

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@


#################################
# Recipes
#################################

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump */*.riscv
	$(if $(libgloss),rm -rf $(libgloss_builddir)/)

.PHONY: default recursivedoublingforchunks-nodes pytorch-listener-nodes
default: $(addsuffix .riscv, $(PROGRAMS)) spiflash.img

# Build all 8 node-specific binaries
recursivedoublingforchunks-nodes: $(addsuffix .riscv, $(RECURSIVE_DOUBLING_NODES))
pytorch-listener-nodes: $(addsuffix .riscv, $(PYTORCH_LISTENER_NODES))

.PHONY: dumps
dumps: $(addsuffix .dump, $(PROGRAMS))
