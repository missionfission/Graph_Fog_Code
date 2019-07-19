// Implementation of a hybrid datapath with both caches and scratchpads.

#include <cstdint>
#include <functional>  // For std::hash
#include <sstream>
#include <string>

#include "base/flags.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "base/logging.hh"
#include "dev/dma_device.hh"
#include "mem/mem_object.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

#include "aladdin/common/DatabaseDeps.h"
#include "aladdin/common/ExecNode.h"
#include "aladdin/common/ScratchpadDatapath.h"
#include "aladdin/common/DynamicEntity.h"
#include "debug/Aladdin.hh"
#include "debug/HybridDatapath.hh"
#include "debug/HybridDatapathVerbose.hh"
#include "aladdin_sys_constants.h"
#include "aladdin_tlb.hh"
#include "HybridDatapath.h"

const unsigned int maxInflightNodes = 100;

HybridDatapath::HybridDatapath(const HybridDatapathParams* params)
    : ScratchpadDatapath(
          params->outputPrefix, params->traceFileName, params->configFileName),
      Gem5Datapath(params,
                   params->acceleratorId,
                   params->executeStandalone,
                   params->system),
      spadPort(this,
               params->system,
               params->maxDmaRequests,
               params->dmaChunkSize,
               params->numDmaChannels,
               params->invalidateOnDmaStore),
      spadMasterId(params->system->getMasterId(this, name() + ".spad")),
      cachePort(this, "cache_port"),
      cacheMasterId(params->system->getMasterId(this, name() + ".cache")),
      acpPort(this, "acp_port"),
      acpMasterId(params->system->getMasterId(this, name() + ".acp")),
      cache_queue(params->cacheQueueSize,
                  params->cacheBandwidth,
                  system->cacheLineSize(),
                  params->cactiCacheQueueConfig),
      enable_stats_dump(params->enableStatsDump),
      acp_enabled(params->enableAcp), use_acp_cache(params->useAcpCache),
      cacheSize(params->cacheSize), cacti_cfg(params->cactiCacheConfig),
      cacheLineSize(system->cacheLineSize()),
      cacheHitLatency(params->cacheHitLatency), cacheAssoc(params->cacheAssoc),
      cache_readEnergy(0), cache_writeEnergy(0), cache_leakagePower(0),
      cache_area(0), dtb(this,
                         params->tlbEntries,
                         params->tlbAssoc,
                         params->tlbHitLatency,
                         params->tlbMissLatency,
                         params->tlbPageBytes,
                         params->numOutStandingWalks,
                         params->tlbBandwidth,
                         params->tlbCactiConfig,
                         params->acceleratorName),
      pipelinedDma(params->pipelinedDma),
      ignoreCacheFlush(params->ignoreCacheFlush), tickEvent(this),
      use_aladdin_debugger(params->useAladdinDebugger), delayedDmaEvent(this),
      reinitializeEvent(this), enterDebuggerEvent(this),
      executedNodesLastTrigger(0) {
  BaseDatapath::use_db = params->useDb;
  BaseDatapath::experiment_name = params->experimentName;
#ifdef USE_DB
  if (use_db) {
    BaseDatapath::setExperimentParameters(experiment_name);
  }
#endif
  float cycle_time = params->cycleTime;
  BaseDatapath::user_params.cycle_time = cycle_time;
  datapath_name = params->acceleratorName;
  cache_queue.initStats(datapath_name + ".cache_queue");
  system->registerAccelerator(accelerator_id, this, accelerator_deps);

  /* In standalone mode, we charge the constant cost of a DMA setup operation
   * up front, instead of per dmaLoad call, or we'll overestimate the setup
   * latency.
   */
  dmaSetupOverhead = params->dmaSetupOverhead / cycle_time;
  if (execute_standalone) {
    initializeDatapath(dmaSetupOverhead);
  }

  /* For the DMA model, compute the cost of a CPU cache flush in terms of
   * accelerator cycles.  Yeah, this is backwards -- we should be doing this
   * from the CPU itself -- but in standalone mode where we don't have a CPU,
   * this is the next best option.
   */
  cacheLineFlushLatency = params->cacheLineFlushLatency / cycle_time;
  cacheLineInvalidateLatency = params->cacheLineFlushLatency / cycle_time;

  if (use_aladdin_debugger)
    adb::init_debugger();
}

HybridDatapath::~HybridDatapath() {
  if (!execute_standalone)
    system->deregisterAccelerator(accelerator_id);
}

void HybridDatapath::clearDatapath() {
  ScratchpadDatapath::clearDatapath();
  dtb.clear();
}

void HybridDatapath::resetCounters() {
  dcache_loads = 0;
  dcache_stores = 0;
  acp_loads = 0;
  acp_stores = 0;
  dma_setup_cycles = 0;
  dtb.resetCounters();
}

void HybridDatapath::initializeDatapath(int delay) {
  // Don't reset stats - the user can reset them manually through the CPU
  // interface or by setting the --enable-stats-dump flag.
  ScratchpadDatapath::clearDatapath();
  bool dddg_built = buildDddg();
  if (!dddg_built) {
    exitSimulation();
    return;
  }
  enterDebuggerIfEnabled();

  globalOptimizationPass();
  prepareForScheduling();
  enterDebuggerIfEnabled();
  adb::execution_status = adb::SCHEDULING;

  num_cycles += delay;
  acc_sim_cycles += delay;
  cachePort.clearRetryPkt();
  startDatapathScheduling(delay);
  if (isReadyMode()) {
    // Ready bits must be explicitly set by the user.
    scratchpad->setReadyBits();
  }
}

void HybridDatapath::startDatapathScheduling(int delay) {
  scheduleOnEventQueue(delay);
}

BaseMasterPort& HybridDatapath::getMasterPort(const std::string& if_name,
                                              PortID idx) {
  if (if_name == "spad_port")
    return getDataPort();
  else if (if_name == "cache_port")
    return getCachePort();
  else if (if_name == "acp_port")
    return getAcpPort();
  else
    return MemObject::getMasterPort(if_name);
}

void HybridDatapath::insertTLBEntry(Addr vaddr, Addr paddr) {
  DPRINTF(HybridDatapath, "Mapping vaddr 0x%x -> paddr 0x%x.\n", vaddr, paddr);
  Addr vpn = vaddr & ~(dtb.pageMask());
  Addr ppn = paddr & ~(dtb.pageMask());
  DPRINTF(
      HybridDatapath, "Inserting TLB entry vpn 0x%x -> ppn 0x%x.\n", vpn, ppn);
  dtb.insertBackupTLB(vpn, ppn);
}

void HybridDatapath::insertArrayLabelToVirtual(const std::string& array_label,
                                               Addr vaddr,
                                               size_t size) {
  DPRINTF(HybridDatapath,
          "Inserting array label mapping %s -> vpn 0x%x, size %d.\n",
          array_label.c_str(), vaddr, size);
  dtb.insertArrayLabelToVirtual(array_label, vaddr, size);
  try {
    user_params.updateArrayParams(array_label, size);
  } catch (UnknownArrayException& e) {
    fatal(e.what());
  }
  user_params.checkOverlappingRanges();
}

void HybridDatapath::resetTrace() {
  DPRINTF(HybridDatapath, "%s: Resetting dynamic trace\n.", name());
  BaseDatapath::resetTrace();
}

void HybridDatapath::eventStep() {
  step();
  scratchpad->step();
}

void HybridDatapath::delayedDmaIssue() {
  // Pop off the front entry, which is the one that this event is being
  // triggered for, and push it onto the issue queue.
  auto waiting_entry = dmaWaitingQueue.front();
  dmaWaitingQueue.pop_front();
  unsigned node_id = waiting_entry.first;
  addDmaNodeToIssueQueue(node_id);

  // "DMA setup latency" in this model means the time required to flush ALL the
  // cache lines being transmitted and invalidate ALL the cache lines for the
  // receiving buffer. Typically, DMA transfer only starts after this process
  // is completely finished. This optimization lets us issue DMA nodes as soon
  // as they have finished their own setup, rather than waiting for everyone's
  // setup to finish.
  if (pipelinedDma) {
    issueDmaRequest(node_id);
    dmaIssueQueue.erase(node_id);
  }

  // Schedule the next delayed DMA event.
  if (!dmaWaitingQueue.empty() && !delayedDmaEvent.scheduled()) {
    auto next_entry = dmaWaitingQueue.front();
    node_id = next_entry.first;
    Tick delay = next_entry.second;
    schedule(delayedDmaEvent, delay + curTick());
    DPRINTF(HybridDatapath,
            "Next setup latency for DMA operation with id %d: %d ticks.\n",
            node_id, delay);
  }

  // In the typical case, after all DMA nodes have exited the waiting queue, we
  // can issue them all at once. No need to wait anymore.
  if (!pipelinedDma && dmaWaitingQueue.empty()) {
    for (auto it = dmaIssueQueue.begin(); it != dmaIssueQueue.end();) {
      issueDmaRequest(*it);
      dmaIssueQueue.erase(it++);
    }
  }
}

void HybridDatapath::printExecutingQueue() {
  std::cout << "Executing queue contents:\n";
  for (auto it = executingQueue.begin(); it != executingQueue.end(); it++)
    std::cout << (*it)->get_node_id() << ", ";
  std::cout << std::endl;
}

void HybridDatapath::stepExecutingQueue() {
  auto it = executingQueue.begin();
  int index = 0;
  while (it != executingQueue.end()) {
    ExecNode* node = *it;
    bool op_satisfied = false;
    if (node->is_memory_op() || node->is_dma_op()) {
      MemoryOpType type;
      try {
        type = getMemoryOpType(node);
      } catch (IllegalHostMemoryAccessException& e) {
        fatal(e.what());
      }

      switch (type) {
        case Register:
          op_satisfied = handleRegisterMemoryOp(node);
          break;
        case Scratchpad:
          op_satisfied = handleSpadMemoryOp(node);
          break;
        case Cache:
          op_satisfied = handleCacheMemoryOp(node);
          break;
        case Dma:
          op_satisfied = handleDmaMemoryOp(node);
          break;
        case ReadyBits:
          op_satisfied = handleReadyBitAccess(node);
          break;
        case ACP:
          op_satisfied = handleAcpMemoryOp(node);
          break;
        default:
          panic("Unsupported memory operation type!");
      }
      if (op_satisfied) {
        markNodeCompleted(it, index);
      }
    } else if (node->is_multicycle_op()) {
      unsigned node_id = node->get_node_id();
      if (inflight_multicycle_nodes.find(node_id) ==
          inflight_multicycle_nodes.end()) {
        inflight_multicycle_nodes[node_id] = node->get_multicycle_latency();
        markNodeStarted(node);
      } else {
        unsigned remaining_cycles = inflight_multicycle_nodes.at(node_id);
        if (remaining_cycles == 1) {
          inflight_multicycle_nodes.erase(node_id);
          markNodeCompleted(it, index);
          op_satisfied = true;
        } else {
          inflight_multicycle_nodes.at(node_id)--;
        }
      }
    } else {
      // Not a memory/fp operation node, so it can be completed in one cycle.
      markNodeStarted(node);
      markNodeCompleted(it, index);
      op_satisfied = true;
    }
    if (!op_satisfied) {
      ++it;
      ++index;
    }
  }
}

void HybridDatapath::retireReturnedMemQEntries() {
  cache_queue.retireReturnedEntries();
}

void HybridDatapath::markNodeStarted(ExecNode* node) {
  BaseDatapath::markNodeStarted(node);
}

bool HybridDatapath::step() {
  executedNodesLastTrigger = executedNodes;
  resetCacheCounters();
  stepExecutingQueue();
  copyToExecutingQueue();
  retireReturnedMemQEntries();
  DPRINTF(Aladdin,
          "[CYCLES]:%d. executedNodes:%d, totalConnectedNodes:%d\n",
          num_cycles,
          executedNodes,
          totalConnectedNodes);
  DPRINTF(Aladdin, "-----------------------------\n");
  if (executedNodesLastTrigger == executedNodes)
    cycles_since_last_node++;
  else
    cycles_since_last_node = 0;
  if (cycles_since_last_node > deadlock_threshold) {
    printExecutingQueue();
    exitSimLoop("Deadlock detected!");
    return false;
  }

  num_cycles++;
  acc_sim_cycles++;
  if (executedNodes < totalConnectedNodes) {
    schedule(tickEvent, clockEdge(Cycles(1)));
    return true;
  } else {
    dumpStats();
    DPRINTF(Aladdin, "Accelerator completed.\n");
    if (execute_standalone) {
      // If in standalone mode, we wait dmaSetupOverhead before the datapath
      // starts, but this cost must be added to this stat. For some reason,
      // this cannot be done until we have started simulation of events,
      // because all stats get cleared before that happens.  So until I figure
      // out how to get around that, it's going to be accounted for at the end.
      dma_setup_cycles += dmaSetupOverhead;

      // In case there are more invocations to run, we will immediately
      // reschedule for initialization.
      clearDatapath();
      schedule(reinitializeEvent, clockEdge(Cycles(1)));
    } else {
      enterDebuggerIfEnabled();
      adb::execution_status = adb::POSTSCHEDULING;
      exitSimulation();
    }
  }
  return false;
}

void HybridDatapath::exitSimulation() {
  if (execute_standalone) {
    system->deregisterAccelerator(accelerator_id);
    if (system->numRunningAccelerators() == 0) {
      exitSimLoop("Aladdin called exit()");
    }
  } else {
    if (enable_stats_dump) {
      std::string exit_reason =
          DUMP_STATS_EXIT_SIM_SIGNAL + datapath_name + " completed.";
      exitSimLoop(exit_reason);
    }
    sendFinishedSignal();
    // Don't clear the datapath right now, since that will destroy stats. Delay
    // clearing the datapath until it gets reinitialized on the next
    // invocation.
  }
}

MemoryOpType HybridDatapath::getMemoryOpType(ExecNode* node) {
  if (node->is_dma_load() || node->is_dma_store() || node->is_dma_fence())
    return Dma;
  if (node->is_set_ready_bits())
    return ReadyBits;

  const std::string& array_label = node->get_array_label();
  auto it = user_params.partition.find(array_label);
  if (it != user_params.partition.end()) {
    PartitionEntry entry = it->second;
    // TODO: Combine the enums MemoryOpType and MemoryType.
    MemoryType memory_type = entry.memory_type;
    switch (memory_type) {
      case spad:
        return Scratchpad;
      case cache:
        return Cache;
      case reg:
        return Register;
      case acp:
        return ACP;
      case host:
        throw IllegalHostMemoryAccessException(node);
    }
  }
  return NumMemoryOpTypes;
}

bool HybridDatapath::handleRegisterMemoryOp(ExecNode* node) {
  markNodeStarted(node);
  std::string reg = node->get_array_label();
  if (node->is_load_op())
    registers.getRegister(reg)->increment_loads();
  else
    registers.getRegister(reg)->increment_stores();
  return true;
}

bool HybridDatapath::handleSpadMemoryOp(ExecNode* node) {
  const std::string& array_name = node->get_array_label();
  unsigned array_name_index = node->get_partition_index();
  MemAccess* mem_access = node->get_mem_access();
  Addr vaddr = mem_access->vaddr;
  bool isLoad = node->is_load_op();
  try {
    bool satisfied = scratchpad->canServicePartition(
                         array_name, array_name_index, vaddr, isLoad);
    if (!satisfied)
      return false;
  } catch (UnknownArrayException& e) {
    fatal("At node %d: tried to access array \"%s\", which does not exist!\n",
          node->get_node_id(), array_name);
  } catch (ArrayAccessException& e) {
    fatal("At node %d: %s\n", node->get_node_id(), e.what());
  }

  markNodeStarted(node);
  if (isLoad) {
    scratchpad->increment_loads(array_name, array_name_index);
  } else {
    size_t size = mem_access->size;
    uint8_t* data = mem_access->data();
    try {
      scratchpad->writeData(array_name, vaddr, data, size);
    } catch (ArrayAccessException& e) {
      fatal("At node %d, attempting to access array %d, trace addr %#x, size "
            "%d: %s", node->get_node_id(), array_name, vaddr, size, e.what());
    }
    scratchpad->increment_stores(array_name, array_name_index);
  }
  return true;
}

bool HybridDatapath::handleReadyBitAccess(ExecNode* node) {
  ReadyBitAccess* access = node->get_ready_bit_access();
  const std::string& array_name = access->array->get_name();
  Addr start_addr = access->vaddr;
  size_t size = access->size;
  bool reset_ready_bits = (access->value == 0);
  try {
    if (reset_ready_bits)
      scratchpad->resetReadyBitRange(array_name, start_addr, size);
    else
      scratchpad->setReadyBitRange(array_name, start_addr, size);
  } catch (UnknownArrayException& e) {
    fatal("At node %d: tried to set ready bits on array \"%s\", which does not "
          "exist!\n", node->get_node_id(), array_name);
  }
  return true;
}

bool HybridDatapath::handleDmaMemoryOp(ExecNode* node) {
  if (node->is_dma_fence())
    return true;

  MemAccessStatus status = Invalid;
  unsigned node_id = node->get_node_id();
  if (inflight_dma_nodes.find(node_id) == inflight_dma_nodes.end())
    inflight_dma_nodes[node_id] = status;
  else
    status = inflight_dma_nodes.at(node_id);
  DmaMemAccess* mem_access = node->get_dma_mem_access();
  if (status == Invalid && inflight_dma_nodes.size() < maxInflightNodes) {
    /* A DMA load needs to be preceded by a cache flush of the buffer being sent,
     * while a DMA store needs to be preceded by a cache invalidate of the
     * receiving buffer. In CPU mode, this should be handled by the CPU; in standalone
     * mode, we add extra latency to the DMA operation here.
     */
    size_t size = mem_access->size;
    bool isLoad = node->is_dma_load();
    unsigned cache_delay_cycles = 0;
    unsigned cache_lines_affected =
        ceil(((float)size) / system->cacheLineSize());
    if (!ignoreCacheFlush && mem_access->src_var->get_name() != "dma_weights") {
      if (isLoad)
        cache_delay_cycles = cache_lines_affected * cacheLineFlushLatency;
      else
        cache_delay_cycles = cache_lines_affected * cacheLineInvalidateLatency;
    }
    dma_setup_cycles += cache_delay_cycles;
    Tick delay = clockPeriod() * cache_delay_cycles;
    if (!delayedDmaEvent.scheduled()) {
      DPRINTF(HybridDatapath,
              "Scheduling DMA %s operation with node id %d with delay %lu\n",
              isLoad ? "load" : "store", node_id, delay);
      schedule(delayedDmaEvent, curTick() + delay);
    }
    inflight_dma_nodes[node_id] = WaitingForDmaSetup;
    dmaWaitingQueue.push_back(std::make_pair(node_id, delay));
    return false;  // DMA op not completed. Move on to the next node.
  } else if (status == Returned) {
    inflight_dma_nodes.erase(node_id);
    return true;  // DMA op completed.
  } else {
    // Still waiting from the cache or for DMA setup to finish. Move on to the next node.
    return false;
  }
}

bool HybridDatapath::handleCacheMemoryOp(ExecNode* node) {
  unsigned node_id = node->get_node_id();
  if (!cache_queue.canIssue()) {
    DPRINTF(HybridDatapathVerbose,
            "Unable to service cache request for node %d: "
            "out of cache queue bandwidth.\n",
            node_id);
    return false;
  }

  MemAccess* mem_access = program.nodes[node_id]->get_mem_access();
  bool isLoad = node->is_load_op();

  // If Aladdin is executing standalone, then the actual addresses that are
  // sent to the memory system of gem5 don't matter as long as there is a 1:1
  // correspondence between them and those in the trace. This mask ensures
  // that even if a trace was generated on a 64 bit system, gem5 will not
  // attempt to access an address outside of a 32-bit address space.
  Addr vaddr = mem_access->vaddr;
  if (isExecuteStandalone())
    vaddr &= ADDR_MASK;

  MemoryQueueEntry* entry = cache_queue.findMatch(vaddr, isLoad);
  if (!entry)
    entry = cache_queue.allocateEntry(vaddr, isLoad);
  if (!entry) {
    DPRINTF(HybridDatapathVerbose,
            "Unable to service new cache request for node %d: "
            "cache queue is full.\n",
            node_id);
    return false;
  } else {
    entry->type = Cache;
  }

  // At this point, we have a valid entry.

  if (entry->status == Invalid) {
    int size = mem_access->size;
    SrcTypes::Variable* var =
        srcManager.get<SrcTypes::Variable>(node->get_array_label());
    if (dtb.canRequestTranslation()) {
      bool issued_tlb_req = false;
      try {
        issued_tlb_req = issueTLBRequestTiming(vaddr, size, isLoad, var);
      } catch (AladdinException& e) {
        fatal("An error occurred during cache access to trace virtual address "
              "%#x at node %d: %s\n",
              vaddr, node_id, e.what());
      }
      if (!issued_tlb_req)
        return false;
      markNodeStarted(node);
      dtb.incrementRequestCounter();
      entry->status = Translating;
      DPRINTF(HybridDatapath,
              "node: %d, trace vaddr = %x, is translating\n",
              node_id,
              vaddr);
    } else {
      DPRINTF(HybridDatapathVerbose, "TLB cannot service node %d\n",
              node->get_node_id());
    }
    return false;
  } else if (entry->status == Translated || entry->status == Retry) {
    Addr paddr = entry->paddr;
    int size = mem_access->size;
    uint8_t* data = mem_access->data();

    IssueResult result =
        issueCacheRequest(vaddr, paddr, size, isLoad, node_id, data);
    if (result == Accepted) {
      entry->status = WaitingFromCache;
      entry->when_issued = curTick();
      dcache_queue_time.sample(entry->when_issued - entry->when_allocated);
      cache_queue.incrementIssuedThisCycle();
      DPRINTF(HybridDatapath,
              "node:%d, vaddr = 0x%x, paddr = 0x%x is accessing cache\n",
              node_id,
              vaddr,
              paddr);
    } else if (result == WillRetry) {
      entry->status = Retry;
    }
    return false;
  } else if (entry->status == Returned) {
    // Memory ops that have returned will be freed at the end of the cycle.
    return true;
  } else if (entry->status == Translating) {
    DPRINTF(HybridDatapathVerbose,
            "Node %d is still waiting for TLB response.\n",
            node->get_node_id());
    return false;
  } else if (entry->status == WaitingFromCache) {
    DPRINTF(HybridDatapathVerbose,
            "Node %d is still waiting for cache response.\n",
            node->get_node_id());
    return false;
  } else {
    panic("Invalid memory status for cache operation.");
  }
}

bool HybridDatapath::handleAcpMemoryOp(ExecNode* node) {
  if (!acp_enabled) {
    fatal("Attempted to perform an ACP access when ACP was not enabled on this "
          "system! Add \"enable_acp = True\" to the gem5.cfg file and try "
          "again.");
  }

  unsigned node_id = node->get_node_id();
  if (!cache_queue.canIssue()) {
    DPRINTF(HybridDatapathVerbose, "Unable to service ACP request for node %d: "
                                   "out of cache queue bandwidth.\n",
            node_id);
    return false;
  }

  MemAccess* mem_access = node->get_mem_access();
  bool isLoad = node->is_load_op();

  // TODO: Use the simulation virtual address. This is the TRACE address, so if
  // we are going to compute aligned addresses, then the trace needs to be
  // generated with aligned addresses too. That's probably not an onerous
  // requirement, but we should not depend on the trace address for anything
  // performance related.
  Addr vaddr = mem_access->vaddr;
  uint8_t* data = mem_access->data();
  int size = mem_access->size;
  if (isExecuteStandalone())
    vaddr &= ADDR_MASK;

  // For loads: we don't need to ask for ownership at all. Instead, we collapse
  // simultaneous requests to the same cache line (aka an MSHR).
  //
  // For stores: we issue separate ownership requests for each request.
  // TODO: ACP is only supposed to accept 2 different transaction sizes (16
  // bytes and 64 bytes), not arbitrary burst lengths.
  MemoryQueueEntry* entry = cache_queue.findMatch(vaddr, isLoad);
  if (!entry)
    entry = cache_queue.allocateEntry(vaddr, isLoad);
  if (!entry) {
    DPRINTF(HybridDatapathVerbose,
            "Unable to service new ACP request for node %d: "
            "cache queue is full.\n",
            node_id);
    return false;
  } else {
    entry->type = ACP;
  }

  if (entry->status == Invalid) {
    if (use_acp_cache) {
      // If we have the middle cache, we don't need to handle ownership requests
      // ourselves. Just go ahead and issue the read or write.
      entry->status = ReadyToIssue;
    } else {
      // We need to explicitly request ownership of a cache line on stores.
      entry->status = isLoad ? ReadyToIssue : ReadyToRequestOwnership;
    }
  }

  if (entry->paddr == 0) {
    // ACP works with physical addresses, which the trace cannot possibly have,
    // so to model this faithfully, we perform the address translation in zero
    // time. Make sure we use the original virtual address.
    SrcTypes::Variable* var =
        srcManager.get<SrcTypes::Variable>(node->get_array_label());
    try {
      AladdinTLBResponse translation =
          getAddressTranslation(vaddr, size, isLoad, var);
      entry->paddr = translation.paddr;
    } catch (AladdinException& e) {
      fatal("An error occurred during ACP access at node %d: %s\n",
            node_id,
            e.what());
    }
  }
  if (!node->started())
    markNodeStarted(node);

  Addr paddr = entry->paddr;
  if (entry->status == ReadyToRequestOwnership) {
    IssueResult result = issueOwnershipRequest(vaddr, paddr, size, node_id);
    if (result == Accepted) {
      entry->status = WaitingForOwnership;
      cache_queue.incrementIssuedThisCycle();
      DPRINTF(HybridDatapath, "node:%d, vaddr = 0x%x, paddr = 0x%x is waiting "
                              "to acquire ownership\n",
              node_id, vaddr, paddr);
    } else if (result == WillRetry) {
      // The port will retry the packet.
      entry->status = Retry;
    } else {
      // The datapath will do the retry, so nothing needs to be done.
    }
    return false;
  } else if (entry->status == ReadyToIssue) {
    IssueResult result =
        issueAcpRequest(vaddr, paddr, size, isLoad, node_id, data);
    if (result == Accepted) {
      entry->status = WaitingFromCache;
      entry->when_issued = curTick();
      acp_queue_time.sample(entry->when_issued - entry->when_allocated);
      cache_queue.incrementIssuedThisCycle();
      DPRINTF(
          HybridDatapath,
          "node:%d, vaddr = 0x%x, paddr = 0x%x is accessing cache via ACP\n",
          node_id, vaddr, paddr);
    } else if (result == WillRetry) {
      entry->status = Retry;
    }
    return false;
  } else if (entry->status == WaitingFromCache) {
    DPRINTF(HybridDatapathVerbose,
            "Node %d is still waiting for L2 cache response.\n",
            node_id);
    return false;
  } else if (entry->status == WaitingForOwnership) {
    DPRINTF(HybridDatapathVerbose,
            "Node %d is still waiting to acquire ownership.\n",
            node_id);
    return false;
  } else if (entry->status == Retry) {
    DPRINTF(HybridDatapathVerbose,
            "Node %d is waiting to be retried.\n",
            node_id);
    return false;
  } else if (entry->status == Returned) {
    // Memory ops that have returned will be freed at the end of the cycle.
    return true;
  } else {
    panic("Invalid memory status for ACP operation.");
  }
}

// Issue a DMA request for memory.
void HybridDatapath::issueDmaRequest(unsigned node_id) {
  ExecNode *node = program.nodes.at(node_id);
  markNodeStarted(node);
  bool isLoad = node->is_dma_load();
  DmaMemAccess* mem_access = node->get_dma_mem_access();
  size_t size = mem_access->size;  // In bytes.
  Addr src_vaddr = mem_access->src_addr;
  Addr dst_vaddr = mem_access->dst_addr;
  if (isExecuteStandalone()) {
    src_vaddr &= ADDR_MASK;
    dst_vaddr &= ADDR_MASK;
  }
  DPRINTF(HybridDatapath,
          "issueDmaRequest: src_addr: %#x, dst_addr: %#x, size: %u\n",
          src_vaddr, dst_vaddr, size);
  /* Assigning the array label can (and probably should) be done in the
   * optimization pass instead of the scheduling pass. */
  Addr accel_addr = isLoad ? dst_vaddr : src_vaddr;

  MemoryType mtype;
  try {
    auto part_it = user_params.getArrayConfig(accel_addr);
    mtype = part_it->second.memory_type;
    node->set_array_label(part_it->first);
  } catch (UnknownArrayException& e) {
    fatal("During DMA at node %d: %s", node_id, e.what());
  }
  fatal_if(!(mtype == spad || mtype == reg),
           "At node %d: DMA request to %s must be to/from a scratchpad or "
           "register, but we got %s\n",
           node_id, node->get_array_label(),
           mtype == cache ? "cache" : mtype == acp ? "acp" : "host");
  if (mtype == spad)
    incrementDmaScratchpadAccesses(node->get_array_label(), size, isLoad);
  else
    registers.getRegister(node->get_array_label())
        ->increment_dma_accesses(isLoad);

  // Update the tracking structures.
  inflight_dma_nodes.at(node_id) = WaitingFromDma;

  // Prepare the transaction.
  MemCmd::Command cmd = isLoad ? MemCmd::ReadReq : MemCmd::WriteReq;
  // Marking the DMA packets as uncacheable ensures they are not snooped by
  // caches.
  Request::Flags flags = Request::UNCACHEABLE;

  /* In order to make DMA stores visible to the rest of the memory hierarchy,
   * we have to do a virtual address translation. However, we do this
   * "invisibly" in zero time.  This is because DMAs are typically initiated by
   * the CPU using physical addresses, but our DMA model initiates transfers
   * from the accelerator, which can't know physical addresses without a
   * translation.
   *
   * Make sure we use the right address for the host memory translation!
   */
  SrcTypes::Variable* var = isLoad ? mem_access->src_var : mem_access->dst_var;
  AladdinTLBResponse translation;
  try {
    translation = getAddressTranslation(
        isLoad ? src_vaddr : dst_vaddr, size, isLoad, var);
  } catch (AladdinException& e) {
    fatal("An error occurred during DMA at node %d: %s\n", node_id, e.what());
  }
  uint8_t* data = new uint8_t[size];
  if (!isLoad) {
    try {
      scratchpad->readData(node->get_array_label(), accel_addr, size, data);
    } catch (ArrayAccessException& e) {
      fatal("When reading from array %s at node %d: %s\n",
            node->get_array_label(), node->get_node_id(), e.what());
    }
  }

  DmaEvent* dma_event = new DmaEvent(this, node_id);
  spadPort.dmaAction(
      cmd, translation.paddr, size, dma_event, data, 0, flags);
}

/* Mark the DMA request node as having completed. */
void HybridDatapath::completeDmaRequest(unsigned node_id) {
  assert(inflight_dma_nodes.find(node_id) != inflight_dma_nodes.end());
  DPRINTF(HybridDatapath, "completeDmaRequest for node %d.\n", node_id);
  inflight_dma_nodes.at(node_id) = Returned;
}

void HybridDatapath::addDmaNodeToIssueQueue(unsigned node_id) {
  assert(program.nodes.at(node_id)->is_dma_op() &&
         "Cannot add non-DMA node to DMA issue queue!");
  DPRINTF(HybridDatapath, "Adding DMA node %d to DMA issue queue.\n", node_id);
  assert(dmaIssueQueue.find(node_id) == dmaIssueQueue.end());
  dmaIssueQueue.insert(node_id);
}

void HybridDatapath::sendFinishedSignal() {
  Request::Flags flags = 0;
  size_t size = 4;  // 32 bit integer.
  uint8_t* data = new uint8_t[size];
  // Set some sentinel value.
  for (int i = 0; i < size; i++)
    data[i] = 0x13;
  auto req =
      std::make_shared<Request>(finish_flag, size, flags, getCacheMasterId());
  req->setContext(context_id);  // Only needed for prefetching.
  MemCmd::Command cmd = MemCmd::WriteReq;
  PacketPtr pkt = new Packet(req, cmd);
  pkt->dataStatic<uint8_t>(data);
  DatapathSenderState* state = new DatapathSenderState(true);
  pkt->pushSenderState(state);

  if (!cachePort.sendTimingReq(pkt)) {
    assert(!cachePort.inRetry());
    cachePort.setRetryPkt(pkt);
    DPRINTF(HybridDatapath, "Sending finished signal failed, retrying.\n");
  } else {
    DPRINTF(HybridDatapath, "Sent finished signal.\n");
  }
}

void HybridDatapath::CachePort::recvReqRetry() {
  assert(inRetry());
  assert(retryPkt->isRequest());
  DPRINTF(HybridDatapath, "recvReqRetry for paddr: %#x \n", retryPkt->getAddr());
  if (sendTimingReq(retryPkt)) {
    DPRINTF(HybridDatapathVerbose, "Retry pass!\n");
    datapath->updateCacheRequestStatusOnRetry(retryPkt);
    clearRetryPkt();
  } else {
    DPRINTF(HybridDatapathVerbose, "Still blocked!\n");
  }
}

bool HybridDatapath::CachePort::recvTimingResp(PacketPtr pkt) {
  DPRINTF(HybridDatapath, "%s: for address: %#x %s\n", __func__, pkt->getAddr(),
          pkt->cmdString());
  if (pkt->isError()) {
    DPRINTF(HybridDatapath,
            "Got error packet back for address: %#x\n",
            pkt->getAddr());
  }
  DatapathSenderState* state =
      dynamic_cast<DatapathSenderState*>(pkt->popSenderState());
  assert(state && "Packet did not contain a DatapathSenderState!");
  if (!state->is_ctrl_signal) {
    datapath->updateCacheRequestStatusOnResp(pkt, state);
  } else {
    DPRINTF(HybridDatapath,
            "recvTimingResp for control signal access: %#x\n",
            pkt->getAddr());
  }
  delete state;
  delete pkt;

  return true;
}

bool HybridDatapath::issueTLBRequestTiming(
    Addr trace_addr, unsigned size, bool isLoad, SrcTypes::Variable* var) {
  DPRINTF(HybridDatapath, "%s for trace addr: %#x\n", __func__, trace_addr);
  PacketPtr data_pkt = createTLBRequestPacket(trace_addr, size, isLoad, var);

  if (dtb.translateTiming(data_pkt)) {
    return true;
  } else {
    // Delete the entire packet; we'll recreate it when we retry the TLB.
    delete data_pkt->popSenderState();
    delete data_pkt;
    return false;
  }
}

AladdinTLBResponse HybridDatapath::getAddressTranslation(
    Addr addr, unsigned size, bool isLoad, SrcTypes::Variable* var) {
  PacketPtr data_pkt = createTLBRequestPacket(addr, size, isLoad, var);
  dtb.translateInvisibly(data_pkt);
  // data_pkt->data now contains the translated address. Make a local copy and
  // return it.
  AladdinTLBResponse translation = *data_pkt->getPtr<AladdinTLBResponse>();

  delete data_pkt->popSenderState();
  delete data_pkt;
  return translation;
}

PacketPtr HybridDatapath::createTLBRequestPacket(
    Addr trace_addr, unsigned size, bool isLoad, SrcTypes::Variable* var) {
  Request::Flags flags = 0;
  // Constructor for physical request only
  auto req =
      std::make_shared<Request>(trace_addr, size, flags, getCacheMasterId());
  MemCmd command = isLoad ? MemCmd::ReadReq : MemCmd::WriteReq;
  PacketPtr data_pkt = new Packet(req, command);

  AladdinTLBResponse* translation = new AladdinTLBResponse();
  AladdinTLB::TLBSenderState* state = new AladdinTLB::TLBSenderState(var);
  data_pkt->dataStatic<AladdinTLBResponse>(translation);
  data_pkt->pushSenderState(state);

  return data_pkt;
}

void HybridDatapath::completeTLBRequest(PacketPtr pkt, bool was_miss) {
  // TLB packets will never be sent out to the memory system, and the only
  // sender states we attach are our own.
  AladdinTLB::TLBSenderState* state =
      safe_cast<AladdinTLB::TLBSenderState*>(pkt->popSenderState());
  DPRINTF(HybridDatapath,
          "completeTLBRequest for paddr: %#x %s\n",
          pkt->getAddr(),
          pkt->cmdString());
  Addr vaddr = pkt->getAddr();  // The trace address.
  bool isLoad = pkt->isRead();
  MemoryQueueEntry* entry = cache_queue.findMatch(vaddr, isLoad);
  panic_if(!entry || entry->status != Translating,
           "Unable to find matching cache queue entry!");

  /* If the TLB missed, we need to retry the complete instruction, as the dcache
   * state may have changed during the waiting time, so we go to Invalid
   * instead of Translated. The next translation request should hit.
   */
  if (was_miss) {
    entry->status = Invalid;
  } else {
    // Translations are actually the complete memory address, not just the page
    // number, because of the trace to virtual address translation.
    Addr paddr = pkt->getPtr<AladdinTLBResponse>()->paddr;
    entry->status = Translated;
    entry->paddr = paddr;
    DPRINTF(
        HybridDatapath, "Translated: vaddr = %#x, paddr = %#x\n", vaddr, paddr);
  }
  delete state;
  delete pkt;
}

HybridDatapath::IssueResult HybridDatapath::issueCacheRequest(Addr vaddr,
                                                              Addr paddr,
                                                              unsigned size,
                                                              bool isLoad,
                                                              unsigned node_id,
                                                              uint8_t* data) {
  return issueCacheOrAcpRequest(
      Cache, vaddr, paddr, size, isLoad, node_id, data);
}

HybridDatapath::IssueResult HybridDatapath::issueAcpRequest(Addr vaddr,
                                                            Addr paddr,
                                                            unsigned size,
                                                            bool isLoad,
                                                            unsigned node_id,
                                                            uint8_t* data) {
  return issueCacheOrAcpRequest(
      ACP, vaddr, paddr, size, isLoad, node_id, data);
}

HybridDatapath::IssueResult HybridDatapath::issueCacheOrAcpRequest(
    MemoryOpType op_type,
    Addr vaddr,
    Addr paddr,
    unsigned size,
    bool isLoad,
    unsigned node_id,
    uint8_t* data) {
  using namespace SrcTypes;
  CachePort& port = op_type == Cache ? cachePort : acpPort;

  if (port.inRetry()) {
    DPRINTF(HybridDatapathVerbose, "%s: %s port in retry.\n",
            op_type == Cache ? "Cache" : "ACP", __func__);
    return DidNotAttempt;
  }

  MasterID id = op_type == Cache ? getCacheMasterId() : getAcpMasterId();

  Request::Flags flags = 0;
  /* To use strided prefetching, we need to include a "PC" so the prefetcher
   * can predict memory behavior similar to how branch predictors work. We
   * don't have real PCs in aladdin so we'll just hash the unique id of the
   * node.  */
  DynamicInstruction inst = program.nodes.at(node_id)->get_dynamic_instruction();
  Addr pc = static_cast<Addr>(std::hash<DynamicInstruction>()(inst));
  auto req = std::make_shared<Request>(paddr, size, flags, id, curTick(), pc);
  /* The context id and thread ids are needed to pass a few assert checks in
   * gem5, but they aren't actually required for the mechanics of the memory
   * checking itself. This has to be set outside of the constructor or the
   * address will be interpreted as a virtual, not physical. */
  req->setContext(context_id);

  MemCmd command = isLoad ? MemCmd::ReadReq : MemCmd::WriteReq;
  PacketPtr data_pkt = new Packet(req, command);
  data_pkt->dataStatic(data);

  DatapathSenderState* state = new DatapathSenderState(node_id, vaddr);
  data_pkt->pushSenderState(state);

  if (!port.sendTimingReq(data_pkt)) {
    assert(!port.inRetry());
    assert(data_pkt->isRequest());
    port.setRetryPkt(data_pkt);
    DPRINTF(HybridDatapathVerbose, "%s port is blocked. Will retry node %d.\n",
            op_type == Cache ? "Cache" : "ACP", node_id);
    return WillRetry;
  }
  return Accepted;
}

HybridDatapath::IssueResult HybridDatapath::issueOwnershipRequest(
    Addr vaddr, Addr paddr, unsigned size, unsigned node_id) {
  using namespace SrcTypes;

  if (acpPort.inRetry()) {
    DPRINTF(HybridDatapathVerbose, "ACP port is blocked. Will retry node %d.\n",
            node_id);
    return DidNotAttempt;
  }

  // In order to obtain ownership of a cache line, we need an aligned access.
  paddr = toCacheLineAddr(paddr);
  size = cacheLineSize;

  RequestPtr req = NULL;
  Request::Flags flags = 0;
  DynamicInstruction inst = program.nodes.at(node_id)->get_dynamic_instruction();
  Addr pc = static_cast<Addr>(std::hash<DynamicInstruction>()(inst));
  req = std::make_shared<Request>(
      paddr, size, flags, getAcpMasterId(), curTick(), pc);
  req->setContext(context_id);

  // The command that comes closest to emulating ACP behavior is ReadExReq,
  // which gives us ownership of this cache line, returns the latest version of
  // the data, and invalidates all other copies. It does not force a writeback
  // to L2, however; that can only be triggered by the cache that is doing the
  // writeback in the first place.
  MemCmd command = MemCmd::ReadExReq;

  PacketPtr data_pkt = new Packet(req, command);
  DatapathSenderState* state = new DatapathSenderState(node_id, vaddr);
  data_pkt->pushSenderState(state);
  uint8_t* data = new uint8_t[size];
  data_pkt->dataDynamic<uint8_t>(data);

  if (!acpPort.sendTimingReq(data_pkt)) {
    // blocked, retry
    assert(!acpPort.inRetry());
    acpPort.setRetryPkt(data_pkt);
    DPRINTF(HybridDatapathVerbose, "ACP is blocked. Will retry later...\n");
    return WillRetry;
  }
  DPRINTF(HybridDatapath,
          "Node id %d invalidation of paddr %#x issued to cache.\n", node_id,
          paddr);

  return Accepted;
}

void HybridDatapath::updateCacheRequestStatusOnResp(
    PacketPtr pkt, DatapathSenderState* state) {
  unsigned node_id = state->node_id;
  Addr vaddr = state->vaddr;
  if (isExecuteStandalone())
    vaddr &= ADDR_MASK;

  DPRINTF(HybridDatapath, "%s for node_id %d, %s\n", __func__, node_id,
          pkt->print());

  bool isLoad = (pkt->cmd == MemCmd::ReadResp);
  MemoryQueueEntry* entry = cache_queue.findMatch(vaddr, isLoad);
  panic_if(!entry, "Did not find a cache queue entry for vaddr %#x\n", vaddr);
  if (entry->status == WaitingFromCache) {
    entry->status = Returned;
    Tick elapsed = curTick() - entry->when_issued;
    DPRINTF(HybridDatapath,
            "setting %s cache queue entry for vaddr %#x to Returned.\n",
            isLoad ? "load" : "store",
            vaddr);
    Stats::Scalar* mem_stat_count;
    Stats::Histogram* mem_stat_latency;
    switch (entry->type) {
      case Cache:
        mem_stat_count = isLoad ? &dcache_loads : &dcache_stores;
        mem_stat_latency = &dcache_latency;
        break;
      case ACP:
        mem_stat_count = isLoad ? &acp_loads : &acp_stores;
        mem_stat_latency = &acp_latency;
        break;
      default:
        panic("Unexpected memory operation type trying to update the cacne "
              "queue!");
    }
    (*mem_stat_count)++;
    mem_stat_latency->sample(elapsed);
  } else if (entry->status == WaitingForOwnership) {
    entry->status = ReadyToIssue;
    DPRINTF(HybridDatapath,
            "setting cache queue entry for address %#x to ReadyToIssue.\n",
            vaddr);
  }
}

void HybridDatapath::updateCacheRequestStatusOnRetry(PacketPtr pkt) {
  DatapathSenderState* state = pkt->findNextSenderState<DatapathSenderState>();
  assert(state && "Retry packet is missing DatapathSenderState!");
  Addr vaddr = state->vaddr;

  // If we receive a ReadExReq or ReadExReq, that means that the trace vaddr
  // associated with the request was for a STORE, and stores do not (currently)
  // get merged into a single cache queue entry.
  bool isLoad = pkt->cmd == MemCmd::ReadReq || pkt->cmd == MemCmd::ReadResp;
  MemoryQueueEntry* entry = cache_queue.findMatch(vaddr, isLoad);
  panic_if(entry->status != Retry,
           "%s called with packet %s, which is not in retry!\n", __func__,
           pkt->print());

  // This packet could be either a request or a response! After calling
  // sendTimingReq(), the original packet could have already been transformed
  // into a response, but the response is not meant to be handled until
  // recvTimingResp() is called (potentially after some delay).
  switch (pkt->cmd.toInt()) {
    case MemCmd::ReadExReq:
    case MemCmd::ReadExResp:
      entry->status = WaitingForOwnership;
      break;
    case MemCmd::ReadReq:
    case MemCmd::ReadResp:
    case MemCmd::WriteReq:
    case MemCmd::WriteLineReq:
    case MemCmd::WriteResp:
      entry->status = WaitingFromCache;
      entry->when_issued = curTick();
      break;
    default:
      panic("%s called with an unexpected packet %s\n",
            __func__, pkt->print());
  }
}

/* Receiving response from DMA. */
bool HybridDatapath::SpadPort::recvTimingResp(PacketPtr pkt) {
  if (pkt->cmd == MemCmd::InvalidateResp) {
    // TODO: We can create a completion event for invalidation responses so
    // that DMA nodes cannot proceed until the invalidations are done.
    return DmaPort::recvTimingResp(pkt);
  }

  // Get the DMA sender state to retrieve node id, so we can get the virtual
  // address (the packet address is possibly physical).
  DmaEvent* event = dynamic_cast<DmaEvent*>(getPacketCompletionEvent(pkt));
  assert(event != nullptr && "Packet completion event is not a DmaEvent object!");

  unsigned node_id = event->get_node_id();
  ExecNode * node = datapath->getProgram().nodes.at(node_id);
  assert(node != nullptr && "DMA node id is invalid!");

  DmaMemAccess* mem_access = node->get_dma_mem_access();
  Addr src_vaddr_base = mem_access->src_addr;
  Addr dst_vaddr_base = mem_access->dst_addr;
  if (datapath->isExecuteStandalone()) {
    src_vaddr_base &= ADDR_MASK;
    dst_vaddr_base &= ADDR_MASK;
  }

  // This will compute the offset of THIS packet from the base address.
  Addr paddr = pkt->getAddr();
  Addr paddr_base = getPacketAddr(pkt);
  Addr pkt_offset = paddr - paddr_base;
  Addr vaddr =
      (node->is_dma_load() ? dst_vaddr_base : src_vaddr_base) + pkt_offset;

  std::string array_label = node->get_array_label();
  unsigned size = pkt->req->getSize(); // in bytes
  DPRINTF(HybridDatapath, "Receiving DMA response for node %d, address %#x "
                          "with label %s and size %d.\n",
          node_id, vaddr, array_label.c_str(), size);
  if (node->is_dma_load() && datapath->isReadyMode())
    datapath->scratchpad->setReadyBitRange(array_label, vaddr, size);

  // For dmaLoads, write the data into the scratchpad.
  if (node->is_dma_load()) {
    uint8_t* data = pkt->getPtr<uint8_t>();
    assert(data != nullptr && "Packet data is null!");
    try {
      datapath->scratchpad->writeData(array_label, vaddr, data, size);
    } catch (ArrayAccessException& e) {
      fatal("When writing to array %s at node %d: %s\n", array_label, node_id,
            e.what());
    }
  }
  return DmaPort::recvTimingResp(pkt);
}

HybridDatapath::DmaEvent::DmaEvent(HybridDatapath* _dpath, unsigned _dma_node_id)
    : Event(Default_Pri, AutoDelete), datapath(_dpath), dma_node_id(_dma_node_id) {}

void HybridDatapath::DmaEvent::process() {
  datapath->completeDmaRequest(dma_node_id);
}

const char* HybridDatapath::DmaEvent::description() const {
  return "Hybrid DMA datapath receiving request event";
}

void HybridDatapath::resetCacheCounters() {
  dtb.resetRequestCounter();
  cache_queue.resetIssuedThisCycle();
}

void HybridDatapath::regStats() {
  using namespace Stats;
  DPRINTF(HybridDatapath, "Registering stats.\n");
  dcache_loads.name("system." + datapath_name + ".total_dcache_loads")
      .desc("Total number of dcache loads")
      .flags(total | nonan);
  dcache_stores.name("system." + datapath_name + ".total_dcache_stores")
      .desc("Total number of dcache stores.")
      .flags(total | nonan);
  dcache_latency.name("system." + datapath_name + ".dcache_latency")
      .init(30)
      .desc("Histogram of dcache total access latency")
      .flags(pdf);
  dcache_queue_time.name("system." + datapath_name + ".dcache_queue_time")
      .init(30)
      .desc("Histogram of time dcache request spent in queue")
      .flags(pdf);
  acp_loads.name("system." + datapath_name + ".total_acp_loads")
      .desc("Total number of ACP loads")
      .flags(total | nonan);
  acp_stores.name("system." + datapath_name + ".total_acp_stores")
      .desc("Total number of ACP stores.")
      .flags(total | nonan);
  acp_latency.name("system." + datapath_name + ".acp_latency")
      .init(30)
      .desc("Histogram of ACP transaction total latency")
      .flags(pdf);
  acp_queue_time.name("system." + datapath_name + ".acp_queue_time")
      .init(30)
      .desc("Histogram of time ACP request spent in queue")
      .flags(pdf);
  dma_setup_cycles.name("system." + datapath_name + ".dma_setup_cycles")
      .desc("Total number of cycles spent on setting up DMA transfers.")
      .flags(total | nonan);
  acc_sim_cycles.name("system." + datapath_name + ".sim_cycles")
      .desc("Total accelerator cycles")
      .flags(total | nonan);
  Gem5Datapath::regStats();
}

#ifdef USE_DB
int HybridDatapath::writeConfiguration(sql::Connection* con) {
  int unrolling_factor, partition_factor;
  bool pipelining;
  getCommonConfigParameters(unrolling_factor, pipelining, partition_factor);

  sql::Statement* stmt = con->createStatement();
  stringstream query;
  query << "insert into configs (id, memory_type, trace_file, "
           "config_file, pipelining, unrolling, partitioning, "
           "max_dma_requests, dma_setup_overhead, cache_size, cache_line_sz, "
           "cache_assoc, cache_hit_latency, "
           "tlb_page_size, tlb_assoc, tlb_miss_latency, "
           "tlb_hit_latency, tlb_max_outstanding_walks, tlb_bandwidth, "
           "tlb_entries) values (";
  query << "NULL"
        << ",\"spad\""
        << ","
        << "\"" << trace_file << "\""
        << ",\"" << config_file << "\"," << pipelining << ","
        << unrolling_factor << "," << partition_factor << ","
        << spadPort.max_req << "," << dmaSetupOverhead << ",\"" << cacheSize
        << "\"," << cacheLineSize << "," << cacheAssoc << "," << cacheHitLatency
        << "," << dtb.getPageBytes() << ","
        << dtb.getAssoc() << "," << dtb.getMissLatency() << ","
        << dtb.getHitLatency() << "," << dtb.getNumOutStandingWalks() << ","
        << dtb.bandwidth << "," << dtb.getNumEntries() << ")";
  stmt->execute(query.str());
  delete stmt;
  // Get the newly added config_id.
  return getLastInsertId(con);
}
#endif

void HybridDatapath::computeCactiResults() {
  // If no caches wer used, then we don't need to invoke CACTI.
  if (cacti_cfg.empty())
    return;
  DPRINTF(
      HybridDatapath, "Invoking CACTI for cache power and area estimates.\n");
  uca_org_t cacti_result = cacti_interface(cacti_cfg);
  // power in mW, energy in pJ, area in mm2
  cache_readEnergy = cacti_result.power.readOp.dynamic * 1e12;
  cache_writeEnergy = cacti_result.power.writeOp.dynamic * 1e12;
  cache_leakagePower = cacti_result.power.readOp.leakage * 1000;
  cache_area = cacti_result.area;

  dtb.computeCactiResults();
  cacti_result.cleanup();
}

void HybridDatapath::getAverageMemPower(unsigned int cycles,
                                        float* avg_power,
                                        float* avg_dynamic,
                                        float* avg_leak) {
  float cache_avg_power = 0, cache_avg_dynamic = 0, cache_avg_leak = 0;
  float spad_avg_power = 0, spad_avg_dynamic = 0, spad_avg_leak = 0;
  float cq_avg_power = 0, cq_avg_dynamic = 0, cq_avg_leak = 0;

  computeCactiResults();
  getAverageCachePower(
      cycles, &cache_avg_power, &cache_avg_dynamic, &cache_avg_leak);
  getAverageSpadPower(
      cycles, &spad_avg_power, &spad_avg_dynamic, &spad_avg_leak);
  getAverageCacheQueuePower(
      cycles, &cq_avg_power, &cq_avg_dynamic, &cq_avg_leak);
  *avg_power = cache_avg_power + spad_avg_power;
  *avg_dynamic = cache_avg_dynamic + spad_avg_dynamic;
  *avg_leak = cache_avg_leak + spad_avg_leak;
}

void HybridDatapath::getAverageCachePower(unsigned int cycles,
                                          float* avg_power,
                                          float* avg_dynamic,
                                          float* avg_leak) {
  float avg_cache_pwr, avg_cache_leak, avg_cache_ac_pwr;
  float avg_tlb_pwr, avg_tlb_leak, avg_tlb_ac_pwr;

  avg_cache_ac_pwr = (dcache_loads.value() * cache_readEnergy +
                      dcache_stores.value() * cache_writeEnergy) /
                     (cycles * user_params.cycle_time);
  avg_cache_leak = cache_leakagePower;
  avg_cache_pwr = avg_cache_ac_pwr + avg_cache_leak;

  dtb.getAveragePower(cycles, user_params.cycle_time, &avg_tlb_pwr,
                      &avg_tlb_ac_pwr, &avg_tlb_leak);

  *avg_dynamic = avg_cache_ac_pwr;
  *avg_leak = avg_cache_leak;
  *avg_power = avg_cache_pwr;

  // TODO: Write this detailed data to the database as well.
  std::string power_file_name = benchName + "_cache_stats.txt";
  std::ofstream power_file(power_file_name.c_str());

  power_file << "system.datapath.dcache.average_pwr " << avg_cache_pwr
             << " # Average dcache dynamic and leakage power." << std::endl;
  power_file << "system.datapath.dcache.dynamic_pwr " << avg_cache_ac_pwr
             << " # Average dcache dynamic power." << std::endl;
  power_file << "system.datapath.dcache.leakage_pwr " << avg_cache_leak
             << " # Average dcache leakage power." << std::endl;
  power_file << "system.datapath.dcache.area " << cache_area
             << " # dcache area." << std::endl;

  power_file << "system.datapath.tlb.average_pwr " << avg_tlb_pwr
             << " # Average tlb dynamic and leakage power." << std::endl;
  power_file << "system.datapath.tlb.dynamic_pwr " << avg_tlb_ac_pwr
             << " # Average tlb dynamic power." << std::endl;
  power_file << "system.datapath.tlb.leakage_pwr " << avg_tlb_leak
             << " # Average tlb leakage power." << std::endl;
  power_file << "system.datapath.tlb.area " << dtb.getArea() << " # tlb area."
             << std::endl;

  power_file.close();
}

void HybridDatapath::getAverageSpadPower(unsigned int cycles,
                                         float* avg_power,
                                         float* avg_dynamic,
                                         float* avg_leak) {
  scratchpad->getAveragePower(cycles, avg_power, avg_dynamic, avg_leak);
}

void HybridDatapath::getAverageCacheQueuePower(unsigned int cycles,
                                               float* avg_power,
                                               float* avg_dynamic,
                                               float* avg_leak) {
  cache_queue.getAveragePower(
      cycles, user_params.cycle_time, avg_power, avg_dynamic, avg_leak);
}

double HybridDatapath::getTotalMemArea() {
  return scratchpad->getTotalArea() + cache_area;
}

Addr HybridDatapath::getBaseAddress(std::string label) {
  return (Addr)BaseDatapath::getBaseAddress(label);
}

// Increment the scratchpad load/store counters based on DMA transfer size.
void HybridDatapath::incrementDmaScratchpadAccesses(std::string array_label,
                                                    unsigned dma_size,
                                                    bool is_dma_load) {
  DPRINTF(
      HybridDatapath, "DMA Accesses: array label %s\n", array_label.c_str());
  if (is_dma_load) {
    scratchpad->increment_dma_loads(array_label, dma_size);
    DPRINTF(HybridDatapath,
            "Increment the dmaLoad accesses of partition %s with size %d. \n",
            array_label,
            dma_size);
  } else {
    scratchpad->increment_dma_stores(array_label, dma_size);
    DPRINTF(HybridDatapath,
            "Increment the dmaStore accesses of partition %s with size %d. \n",
            array_label,
            dma_size);
  }
}

////////////////////////////////////////////////////////////////////////////
//
//  The SimObjects we use to get the Datapath information into the simulator
//
////////////////////////////////////////////////////////////////////////////

HybridDatapath* HybridDatapathParams::create() {
  return new HybridDatapath(this);
}
