package chipyard

import org.chipsalliance.cde.config.{Config}

// ---------------------
// BOOM Configs
// ---------------------

class SmallBoomConfig extends Config(
  new boom.common.WithNSmallBooms(1) ++                          // small boom config
  new chipyard.config.AbstractConfig)

class MediumBoomConfig extends Config(
  new boom.common.WithNMediumBooms(1) ++                         // medium boom config
  new chipyard.config.AbstractConfig)

class LargeBoomConfig extends Config(
  new boom.common.WithNLargeBooms(1) ++                          // large boom config
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class MegaBoomConfig extends Config(
  new boom.common.WithNMegaBooms(1) ++                           // mega boom config
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class DualSmallBoomConfig extends Config(
  new boom.common.WithNSmallBooms(2) ++                          // 2 boom cores
  new chipyard.config.AbstractConfig)

class Cloned64MegaBoomConfig extends Config(
  new boom.common.WithCloneBoomTiles(63, 0) ++
  new boom.common.WithNMegaBooms(1) ++                           // mega boom config
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class LoopbackNICLargeBoomConfig extends Config(
  new chipyard.harness.WithLoopbackNIC ++                        // drive NIC IOs with loopback
  new icenet.WithIceNIC ++                                       // build a NIC
  new boom.common.WithNLargeBooms(1) ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class MediumBoomCosimConfig extends Config(
  new chipyard.harness.WithCospike ++                            // attach spike-cosim
  new chipyard.config.WithTraceIO ++                             // enable the traceio
  new boom.common.WithNMediumBooms(1) ++
  new chipyard.config.AbstractConfig)

class dmiMediumBoomConfig extends Config(
  new chipyard.harness.WithSerialTLTiedOff ++                    // don't attach anything to serial-tl
  new chipyard.config.WithDMIDTM ++                              // have debug module expose a clocked DMI port
  new boom.common.WithNMediumBooms(1) ++
  new chipyard.config.AbstractConfig)

class dmiMediumBoomCosimConfig extends Config(
  new chipyard.harness.WithCospike ++                            // attach spike-cosim
  new chipyard.config.WithTraceIO ++                             // enable the traceio
  new chipyard.harness.WithSerialTLTiedOff ++                    // don't attach anythint to serial-tl
  new chipyard.config.WithDMIDTM ++                              // have debug module expose a clocked DMI port
  new boom.common.WithNMediumBooms(1) ++
  new chipyard.config.AbstractConfig)

class PacketModifierConfig extends Config(
  new icenet.collective.WithPacketModifier ++         // Add the custom module
  new chipyard.harness.WithPacketModifierHarness ++   // Add the custom harness for the module
  new icenet.WithIceNIC ++                            // Add the NIC
  new boom.common.WithNLargeBooms(1) ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class RecursiveDoublingConfig extends Config(
  new icenet.collective.WithRecursiveDoubling ++         // Add the custom module
  new chipyard.harness.WithRecursiveDoublingHarness ++   // Add the custom harness for the module
  new icenet.WithIceNIC ++                            // Add the NIC
  new boom.common.WithNLargeBooms(1) ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class SimpleDmaControllerConfig extends Config(
  new chipyard.harness.WithSimpleDmaControllerHarness ++ // Add the custom harness binder
  new icenet.WithIceNIC ++                         // Add the NIC itself
  new boom.common.WithNLargeBooms(1) ++            // Add a BOOM core
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)

class RecursiveDoublingWithDMAConfig extends Config(
  // numMemoryBlocks is the single memory knob: TLRAM span = numMemoryBlocks * bytesPerChunk (1KB).
  // 1024 -> 1MB (baseline, unchanged). Edit this (keep a power of 2) to shrink memory / force reuse.
  // EnableStats = timing counters + the one-line STATE_SUMMARY per collective (keep on for DSE runs).
  // EnableDebug = functional traces + the per-chunk STATE_CYCLES line; kept on only to cross-check the
  // summary against the last per-chunk line during verification -- set it false for actual sweeps.
  new icenet.collective.WithRecursiveDoublingWithDMA(EnableDebug = true, EnableStats = true, maxChunks = 64, numMemoryBlocks = 16) ++  // Add the custom module (Verilator: use RegInit)
  new chipyard.harness.WithRecursiveDoublingWithDMAHarness ++   // Add the custom harness for the module
  new icenet.WithIceNIC ++                            // Add the NIC
  new boom.common.WithNLargeBooms(1) ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.config.AbstractConfig)
