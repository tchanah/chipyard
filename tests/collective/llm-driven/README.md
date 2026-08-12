# llm-driven — Verilator latency measurement tester

Single-instance Verilator tester to measure **relative latency (cycles)** of the
`RecursiveDoublingWithDMA` accelerator as you manually vary two knobs:

| Knob | Where | Effect |
|------|-------|--------|
| `numMemoryBlocks` | `generators/chipyard/src/main/scala/config/BoomConfigs.scala` (the `WithRecursiveDoublingWithDMA(...)` call) | TLRAM size = `numMemoryBlocks × 1 KB` (derived in `NIC.scala`). Keep a **power of 2**. Shrinking it forces block reuse. |
| `MAX_CHUNKS_PER_LEVEL` | this tester, via `-D LLM_CHUNKS=<N>` | chunks actually sent per level (data volume). Must stay **< hardware `maxChunks`** (config) or chunk indices silently alias. |

Packet size is fixed at **1 KB** (1 block = 1 KB), so `numMemoryBlocks` maps 1:1 to KB of TLRAM.

## Build

```bash
cmake -S tests -B tests/build -D LLM_CHUNKS=4 -D LLM_SETS=1   # -DMAX_CHUNKS_PER_LEVEL=4 -DNUM_TEST_SETS=1
cmake --build tests/build --target recursivedoubling_llm
```
Defaults: `LLM_CHUNKS=4`, `LLM_SETS=2` (also `LLM_LAG=2`, `LLM_JITTER=0`, `LLM_SEED=1`,
`LLM_FORMAT=FP_FORMAT_DLFLOAT` — see `tests/CMakeLists.txt`). These are real CMake cache variables, not
raw `-D` defines, so re-running the `cmake -S ... -D` configure step with a new value automatically
triggers a rebuild of `recursivedoubling_llm` — no `make clean` needed between sweeps.

## Run

```bash
cd sims/verilator
make CONFIG=RecursiveDoublingWithDMAConfig                # rebuild only after a Scala/config change
make CONFIG=RecursiveDoublingWithDMAConfig \
     BINARY=../../tests/build/recursivedoubling_llm.riscv \
     run-binary 2>&1 | tee logs/llm_blk<N>_chunks<M>
grep -E "LATENCY_CSV|SUCCESS|DEADLOCK" logs/llm_blk<N>_chunks<M>
```

## Reading the result

Each collective prints one greppable line:
```
LATENCY_CSV set=1 collId=0x1000 numChunksPerLevel=4 totalChunks=4 TOTAL_CYCLES=… CYCLES_PER_CHUNK=…
```
- **`TOTAL_CYCLES`** — first packet → last Level-4 response (end-to-end; includes NIC + software pacing).
- **`CYCLES_PER_CHUNK`** — `TOTAL_CYCLES / totalChunks`, a steady-state proxy that amortizes startup.

The send order is **fixed (natural order)** — deliberately not randomized — so cycle deltas come from the
config, not packet ordering.

### Three regimes (vary `numMemoryBlocks` down at a fixed `CHUNKS`)
1. **Ample** (blocks ≫ working set) — cycles flat, no reuse.
2. **Reuse pressure** (blocks below ~4× working set) — `TOTAL_CYCLES` rises as the allocator stalls.
3. **Deadlock floor** (blocks too few to progress one chunk) — prints `DEADLOCK/STALL` and fails. This is a
   *distinct* outcome from "slow but passes"; back off `numMemoryBlocks` up or lower `CHUNKS`.

The "best ratio" is just above the knee: smallest TLRAM that still sits in regime 1.

> The per-state cycle breakdown (memory-stall vs DMA vs FP-add) that explains *why* cycles rose is printed
> by the RTL counters added in `RecursiveDoublingWithDMA.scala` (Stage C), gated on `EnableDebug`.
